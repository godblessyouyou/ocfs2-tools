/* -*- mode: c; c-basic-offset: 8; -*-
 * vim: noexpandtab sw=8 ts=8 sts=0:
 *
 * ocfs2ne_features.c
 *
 * ocfs2 tune utility for adding and removing features.
 *
 * Copyright (C) 2004, 2008 Oracle.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <inttypes.h>

#include "ocfs2/ocfs2.h"

#include "libocfs2ne.h"
#include "libocfs2ne_err.h"


struct feature_op_state {
	struct tunefs_operation	*fo_op;		/* A pointer back to
						   features_op so we can
						   set to_open_flags based
						   on the features we're
						   changing */
	ocfs2_fs_options	fo_feature_set;	/* Features to enabling */
	ocfs2_fs_options	fo_reverse_set;	/* Features to disable */
	struct tunefs_feature	*fo_features[];	/* List of known features */
};

extern struct tunefs_feature backup_super_feature;
extern struct tunefs_feature extended_slotmap_feature;
extern struct tunefs_feature local_feature;
extern struct tunefs_feature sparse_files_feature;
extern struct tunefs_feature unwritten_extents_feature;

struct feature_op_state feature_state = {
	.fo_features = {
		&backup_super_feature,
		&extended_slotmap_feature,
		&local_feature,
		&sparse_files_feature,
		&unwritten_extents_feature,
		NULL,
	},
};


static struct tunefs_feature *find_feature(struct feature_op_state *state,
					   ocfs2_fs_options *feature)
{
	int i;
	struct tunefs_feature *feat = NULL;

	for (i = 0; state->fo_features[i]; i++) {
		feat = state->fo_features[i];
		if (!memcmp(&feat->tf_feature, feature,
			    sizeof(ocfs2_fs_options))) {
			break;
		}
		feat = NULL;
	}

	return feat;
}


struct check_supported_context {
	struct feature_op_state		*sc_state;
	char				*sc_string;
	int				sc_error;
	enum tunefs_feature_action	sc_action;
};

/* Order doesn't actually matter here.  We just want to know that
 * tunefs supports this feature */
static int check_supported_func(ocfs2_fs_options *feature, void *user_data)
{
	int rc = 1;
	struct check_supported_context *ctxt = user_data;
	struct feature_op_state *state = ctxt->sc_state;
	struct tunefs_feature *feat = find_feature(state, feature);

	if (!feat) {
		errorf("One or more of the features in \"%s\" are not "
		       "supported by this program\n",
		       ctxt->sc_string);
		ctxt->sc_error = 1;
		goto out;
	}

	switch (ctxt->sc_action) {
		case FEATURE_ENABLE:
			if (!feat->tf_enable) {
				errorf("This program does not "
				       "support enabling feature "
				       "\"%s\"\n",
				       feat->tf_name);
				ctxt->sc_error = 1;
				goto out;
			}
			break;

		case FEATURE_DISABLE:
			if (!feat->tf_disable) {
				errorf("This program does not "
				       "support disabling feature "
				       "\"%s\"\n",
				       feat->tf_name);
				ctxt->sc_error = 1;
				goto out;
			}
			break;

		case FEATURE_NOOP:
			verbosef(VL_APP,
				 "Should have gotten a NOOP "
				 "action for feature \"%s\"\n",
				 feat->tf_name);
			rc = 0;
			goto out;
			break;

		default:
			errorf("Unknown action for feature \"%s\"\n",
			       feat->tf_name);
			ctxt->sc_error = 1;
			goto out;
			break;
	}

	verbosef(VL_APP, "%s feature \"%s\"\n",
		 ctxt->sc_action == FEATURE_ENABLE ?
		 "Enabling" : "Disabling",
		 feat->tf_name);
	feat->tf_action = ctxt->sc_action;
	state->fo_op->to_open_flags |= feat->tf_open_flags;

	rc = 0;

out:
	return rc;
}

static int features_parse_option(char *arg, void *user_data)
{
	int rc = 1;
	errcode_t err;
	struct feature_op_state *state = user_data;
	struct check_supported_context ctxt = {
		.sc_state = state,
		.sc_string = arg,
	};

	if (!arg) {
		errorf("No features specified\n");
		goto out;
	}

	err = ocfs2_parse_feature(arg, &state->fo_feature_set,
				  &state->fo_reverse_set);
	if (err) {
		tcom_err(err,
			 "while parsing feature options \"%s\"",
			 arg);
		goto out;
	}

	ctxt.sc_action = FEATURE_ENABLE;
	ocfs2_feature_foreach(&state->fo_feature_set, check_supported_func,
			      &ctxt);
	if (ctxt.sc_error)
		goto out;

	ctxt.sc_action = FEATURE_DISABLE;
	ocfs2_feature_reverse_foreach(&state->fo_reverse_set,
				      check_supported_func,
				      &ctxt);
	if (ctxt.sc_error)
		goto out;

	rc = 0;

out:
	return rc;
}


struct run_features_context {
	struct feature_op_state	*rc_state;
	ocfs2_filesys		*rc_fs;
	int			rc_flags;
	int			rc_error;
};

static int run_feature_func(ocfs2_fs_options *feature, void *user_data)
{
	struct run_features_context *ctxt = user_data;
	struct tunefs_feature *feat = find_feature(ctxt->rc_state,
						   feature);

	return tunefs_feature_run(ctxt->rc_fs, ctxt->rc_flags, feat);
}

static int features_run(ocfs2_filesys *fs, int flags, void *user_data)
{
	struct feature_op_state *state = user_data;
	struct run_features_context ctxt = {
		.rc_state = state,
		.rc_fs = fs,
		.rc_flags = flags,
	};

	ocfs2_feature_reverse_foreach(&state->fo_reverse_set,
				      run_feature_func,
				      &ctxt);
	if (!ctxt.rc_error)
		ocfs2_feature_foreach(&state->fo_feature_set,
				      run_feature_func,
				      &ctxt);

	return ctxt.rc_error;
}


DEFINE_TUNEFS_OP(features,
		 "Usage: ocfs2ne_features [opts] <device> <features>\n",
		 0,
		 features_parse_option,
		 features_run,
		 &feature_state);

int main(int argc, char *argv[])
{
	feature_state.fo_op = &features_op;
	return tunefs_main(argc, argv, &features_op);
}

