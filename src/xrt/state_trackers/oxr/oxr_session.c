// Copyright 2018-2019, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Holds session related functions.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup oxr_main
 */


#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <inttypes.h>

#include "util/u_debug.h"
#include "util/u_misc.h"
#include "util/u_time.h"

#include "math/m_api.h"

#include "xrt/xrt_device.h"
#include "xrt/xrt_gfx_xlib.h"
#include "xrt/xrt_gfx_vk.h"

#include "oxr_objects.h"
#include "oxr_logger.h"
#include "oxr_two_call.h"
#include "oxr_handle.h"
#include "oxr_chain.h"
#include "oxr_api_verify.h"


DEBUG_GET_ONCE_BOOL_OPTION(dynamic_prediction, "OXR_DYNAMIC_PREDICTION", true)
DEBUG_GET_ONCE_NUM_OPTION(ipd, "OXR_DEBUG_IPD_MM", 63)
DEBUG_GET_ONCE_NUM_OPTION(prediction_ms, "OXR_DEBUG_PREDICTION_MS", 11)

static bool
is_running(XrSessionState state)
{
	switch (state) {
	case XR_SESSION_STATE_SYNCHRONIZED: return true;
	case XR_SESSION_STATE_VISIBLE: return true;
	case XR_SESSION_STATE_FOCUSED: return true;
	case XR_SESSION_STATE_STOPPING: return true;
	default: return false;
	}
}

static bool
should_render(XrSessionState state)
{
	switch (state) {
	case XR_SESSION_STATE_VISIBLE: return true;
	case XR_SESSION_STATE_FOCUSED: return true;
	case XR_SESSION_STATE_STOPPING: return true;
	default: return false;
	}
}

static void
oxr_session_change_state(struct oxr_logger *log,
                         struct oxr_session *sess,
                         XrSessionState state)
{
	oxr_event_push_XrEventDataSessionStateChanged(log, sess, state, 0);
	sess->state = state;
}

XrResult
oxr_session_enumerate_formats(struct oxr_logger *log,
                              struct oxr_session *sess,
                              uint32_t formatCapacityInput,
                              uint32_t *formatCountOutput,
                              int64_t *formats)
{
	struct xrt_compositor *xc = sess->compositor;
	if (formatCountOutput == NULL) {
		return oxr_error(log, XR_ERROR_VALIDATION_FAILURE,
		                 "(formatCountOutput)");
	}
	if (xc == NULL) {
		if (formatCountOutput != NULL) {
			*formatCountOutput = 0;
		}
		return oxr_session_success_result(sess);
	}

	OXR_TWO_CALL_HELPER(log, formatCapacityInput, formatCountOutput,
	                    formats, xc->num_formats, xc->formats,
	                    oxr_session_success_result(sess));
}

XrResult
oxr_session_begin(struct oxr_logger *log,
                  struct oxr_session *sess,
                  const XrSessionBeginInfo *beginInfo)
{
	if (is_running(sess->state)) {
		return oxr_error(log, XR_ERROR_SESSION_RUNNING,
		                 " session is already running");
	}
	struct xrt_compositor *xc = sess->compositor;
	if (xc != NULL) {
		XrViewConfigurationType view_type =
		    beginInfo->primaryViewConfigurationType;

		if (view_type != sess->sys->view_config_type) {
			/*! @todo we only support a single view config type per
			 * system right now */
			return oxr_error(
			    log, XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED,
			    " view configuration type not supported");
		}

		xrt_comp_begin_session(xc, (enum xrt_view_type)beginInfo
		                               ->primaryViewConfigurationType);
	}

	oxr_session_change_state(log, sess, XR_SESSION_STATE_SYNCHRONIZED);
	oxr_session_change_state(log, sess, XR_SESSION_STATE_VISIBLE);
	oxr_session_change_state(log, sess, XR_SESSION_STATE_FOCUSED);

	return oxr_session_success_result(sess);
}

XrResult
oxr_session_end(struct oxr_logger *log, struct oxr_session *sess)
{
	struct xrt_compositor *xc = sess->compositor;

	if (!is_running(sess->state)) {
		return oxr_error(log, XR_ERROR_SESSION_NOT_RUNNING,
		                 " session is not running");
	}
	if (sess->state != XR_SESSION_STATE_STOPPING) {
		return oxr_error(log, XR_ERROR_SESSION_NOT_STOPPING,
		                 " session is not stopping");
	}

	if (xc != NULL) {
		if (sess->frame_started) {
			xrt_comp_discard_frame(xc);
			sess->frame_started = false;
		}

		xrt_comp_end_session(xc);
	}

	oxr_session_change_state(log, sess, XR_SESSION_STATE_IDLE);
	if (sess->exiting) {
		oxr_session_change_state(log, sess, XR_SESSION_STATE_EXITING);
	} else {
		oxr_session_change_state(log, sess, XR_SESSION_STATE_READY);
	}
	return oxr_session_success_result(sess);
}

XrResult
oxr_session_request_exit(struct oxr_logger *log, struct oxr_session *sess)
{
	if (!is_running(sess->state)) {
		return oxr_error(log, XR_ERROR_SESSION_NOT_RUNNING,
		                 " session is not running");
	}
	if (sess->state == XR_SESSION_STATE_FOCUSED) {
		oxr_session_change_state(log, sess, XR_SESSION_STATE_VISIBLE);
	}
	if (sess->state == XR_SESSION_STATE_VISIBLE) {
		oxr_session_change_state(log, sess,
		                         XR_SESSION_STATE_SYNCHRONIZED);
	}
	//! @todo start fading out the app.
	oxr_session_change_state(log, sess, XR_SESSION_STATE_STOPPING);
	sess->exiting = true;
	return oxr_session_success_result(sess);
}

void
oxr_session_poll(struct oxr_session *sess)
{
	struct xrt_compositor *xc = sess->compositor;
	(void)xc; // TODO: dispatch to compositor
}


XrResult
oxr_session_get_view_pose_at(struct oxr_logger *log,
                             struct oxr_session *sess,
                             XrTime at_time,
                             struct xrt_pose *pose)
{
	// @todo This function needs to be massively expanded to support all
	//       use cases this drive. The main use of this function is to get
	//       either the predicted position of the headset device. Right now
	//       it only returns the current position. But it must also deal
	//       with past values are allowed by the spec. See displayTime
	//       argument on the xrLocateViews function. It will also drive
	//       the function xrLocateSpace view using the view space.
	// @todo If using orientation tracking only implement a neck model to
	//       get at least a slightly better position.

	struct xrt_device *xdev = sess->sys->head;
	struct xrt_space_relation relation;
	uint64_t timestamp;

	// Applies the offset in the function.
	oxr_xdev_get_relation_at(log, sess->sys->inst, xdev,
	                         XRT_INPUT_GENERIC_HEAD_POSE, at_time,
	                         &timestamp, &relation);

	// clang-format off
	*pose = relation.pose;

	bool valid_vel = (relation.relation_flags & XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT) != 0;
	// clang-format on


	if (valid_vel) {
		//! @todo Forcing a fixed amount of prediction for now since
		//! devices don't tell us timestamps yet.
		int64_t ns_diff = at_time - timestamp;
		float interval;
		if (debug_get_bool_option_dynamic_prediction()) {
			interval =
			    time_ns_to_s(ns_diff) + sess->static_prediction_s;
		} else {
			interval = sess->static_prediction_s;
		}

		struct xrt_quat predicted;
		math_quat_integrate_velocity(&pose->orientation,
		                             &relation.angular_velocity,
		                             interval, &predicted);

		if (sess->sys->inst->debug_views) {
			fprintf(stderr,
			        "\toriginal quat = {%f, %f, %f, %f}   "
			        "(time requested: %" PRIi64
			        ", Interval %" PRIi64
			        " nsec, with "
			        "static interval %f s)\n",
			        pose->orientation.x, pose->orientation.y,
			        pose->orientation.z, pose->orientation.w,
			        at_time, ns_diff, interval);
		}

		pose->orientation = predicted;
	}

	return oxr_session_success_result(sess);
}

void
print_view_fov(struct oxr_session *sess,
               uint32_t index,
               const struct xrt_fov *fov)
{
	if (!sess->sys->inst->debug_views) {
		return;
	}

	fprintf(stderr, "\tviews[%i].fov = {%f, %f, %f, %f}\n", index,
	        fov->angle_left, fov->angle_right, fov->angle_up,
	        fov->angle_down);
}

void
print_view_pose(struct oxr_session *sess,
                uint32_t index,
                const struct xrt_pose *pose)
{
	if (!sess->sys->inst->debug_views) {
		return;
	}

	fprintf(stderr, "\tviews[%i].pose = {{%f, %f, %f, %f}, {%f, %f, %f}}\n",
	        index, pose->orientation.x, pose->orientation.y,
	        pose->orientation.z, pose->orientation.w, pose->position.x,
	        pose->position.y, pose->position.z);
}


XrResult
oxr_session_views(struct oxr_logger *log,
                  struct oxr_session *sess,
                  const XrViewLocateInfo *viewLocateInfo,
                  XrViewState *viewState,
                  uint32_t viewCapacityInput,
                  uint32_t *viewCountOutput,
                  XrView *views)
{
	struct xrt_device *xdev = sess->sys->head;
	struct oxr_space *baseSpc = XRT_CAST_OXR_HANDLE_TO_PTR(
	    struct oxr_space *, viewLocateInfo->space);
	uint32_t num_views = 2;

	// Does this apply for all calls?
	if (!baseSpc->is_reference) {
		viewState->viewStateFlags = 0;
		return oxr_session_success_result(sess);
	}

	// Start two call handling.
	if (viewCountOutput != NULL) {
		*viewCountOutput = num_views;
	}
	if (viewCapacityInput == 0) {
		return oxr_session_success_result(sess);
	}
	if (viewCapacityInput < num_views) {
		return oxr_error(log, XR_ERROR_SIZE_INSUFFICIENT,
		                 "(viewCapacityInput == %u) need %u",
		                 viewCapacityInput, num_views);
	}
	// End two call handling.

	if (sess->sys->inst->debug_views) {
		fprintf(stderr, "%s\n", __func__);
		fprintf(stderr, "\tviewLocateInfo->displayTime %" PRIu64 "\n",
		        viewLocateInfo->displayTime);
	}

	// Get the viewLocateInfo->space to view space relation.
	struct xrt_space_relation pure_relation;
	oxr_space_ref_relation(log, sess, XR_REFERENCE_SPACE_TYPE_VIEW,
	                       baseSpc->type, viewLocateInfo->displayTime,
	                       &pure_relation);

	struct xrt_pose pure = pure_relation.pose;

	// @todo the fov information that we get from xdev->hmd->views[i].fov is
	//       not properly filled out in oh_device.c, fix before wasting time
	//       on debugging weird rendering when adding stuff here.

	for (uint32_t i = 0; i < num_views; i++) {
		//! @todo Do not hardcode IPD.
		struct xrt_vec3 eye_relation = {
		    sess->ipd_meters,
		    0.0f,
		    0.0f,
		};
		struct xrt_pose view_pose;

		// Get the per view pose from the device.
		xdev->get_view_pose(xdev, &eye_relation, i, &view_pose);

		// Do the magical space relation dance here.
		math_pose_openxr_locate(&view_pose, &pure, &baseSpc->pose,
		                        (struct xrt_pose *)&views[i].pose);

		// Copy the fov information directly from the device.
		union {
			struct xrt_fov xrt;
			XrFovf oxr;
		} safe_copy = {0};
		safe_copy.xrt = xdev->hmd->views[i].fov;
		views[i].fov = safe_copy.oxr;

		print_view_fov(sess, i, (struct xrt_fov *)&views[i].fov);
		print_view_pose(sess, i, (struct xrt_pose *)&views[i].pose);
	}

	// @todo Add tracking bit once we have them.
	viewState->viewStateFlags = 0;
	viewState->viewStateFlags |= XR_VIEW_STATE_POSITION_VALID_BIT;
	viewState->viewStateFlags |= XR_VIEW_STATE_ORIENTATION_VALID_BIT;

	return oxr_session_success_result(sess);
}

XrResult
oxr_session_frame_wait(struct oxr_logger *log,
                       struct oxr_session *sess,
                       XrFrameState *frameState)
{
	if (!is_running(sess->state)) {
		return oxr_error(log, XR_ERROR_SESSION_NOT_RUNNING,
		                 " session is not running");
	}

	//! @todo this should be carefully synchronized, because there may be
	//! more than one session per instance.
	XRT_MAYBE_UNUSED timepoint_ns now =
	    time_state_get_now_and_update(sess->sys->inst->timekeeping);

	struct xrt_compositor *xc = sess->compositor;
	if (xc == NULL) {
		frameState->shouldRender = XR_FALSE;
		return oxr_session_success_result(sess);
	}

	uint64_t predicted_display_time;
	uint64_t predicted_display_period;
	xrt_comp_wait_frame(xc, &predicted_display_time,
	                    &predicted_display_period);

	if ((int64_t)predicted_display_time <= 0) {
		return oxr_error(log, XR_ERROR_RUNTIME_FAILURE,
		                 " got a negative display time '%" PRIi64 "'",
		                 (int64_t)predicted_display_time);
	}

	frameState->shouldRender = should_render(sess->state);
	frameState->predictedDisplayPeriod = predicted_display_period;
	frameState->predictedDisplayTime = time_state_monotonic_to_ts_ns(
	    sess->sys->inst->timekeeping, predicted_display_time);

	if (frameState->predictedDisplayTime <= 0) {
		return oxr_error(
		    log, XR_ERROR_RUNTIME_FAILURE,
		    " time_state_monotonic_to_ts_ns returned '%" PRIi64 "'",
		    frameState->predictedDisplayTime);
	}

	return oxr_session_success_result(sess);
}

XrResult
oxr_session_frame_begin(struct oxr_logger *log, struct oxr_session *sess)
{
	if (!is_running(sess->state)) {
		return oxr_error(log, XR_ERROR_SESSION_NOT_RUNNING,
		                 " session is not running");
	}

	struct xrt_compositor *xc = sess->compositor;

	XrResult ret;
	if (sess->frame_started) {
		ret = XR_FRAME_DISCARDED;
		if (xc != NULL) {
			xrt_comp_discard_frame(xc);
		}
	} else {
		ret = oxr_session_success_result(sess);
		sess->frame_started = true;
	}
	if (xc != NULL) {
		xrt_comp_begin_frame(xc);
	}

	return ret;
}

static enum xrt_blend_mode
oxr_blend_mode_to_xrt(XrEnvironmentBlendMode blend_mode)
{
	// clang-format off
	switch (blend_mode) {
	case XR_ENVIRONMENT_BLEND_MODE_OPAQUE: return XRT_BLEND_MODE_OPAQUE;
	case XR_ENVIRONMENT_BLEND_MODE_ADDITIVE: return XRT_BLEND_MODE_ADDITIVE;
	case XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND: return XRT_BLEND_MODE_ALPHA_BLEND;
	default: return (enum xrt_blend_mode)0;
	}
	// clang-format on
}

static XrResult
verify_space(struct oxr_logger *log, uint32_t layer_index, XrSpace space)
{
	if (space == XR_NULL_HANDLE) {
		return oxr_error(
		    log, XR_ERROR_VALIDATION_FAILURE,
		    "(frameEndInfo->layers[%u]->space == "
		    "XR_NULL_HANDLE) XrSpace must not be XR_NULL_HANDLE",
		    layer_index);
	}

	return XR_SUCCESS;
}

static XrResult
verify_quad_layer(struct xrt_compositor *xc,
                  struct oxr_logger *log,
                  uint32_t layer_index,
                  XrCompositionLayerQuad *quad,
                  struct xrt_device *head,
                  uint64_t timestamp)
{
	struct oxr_swapchain *sc = XRT_CAST_OXR_HANDLE_TO_PTR(
	    struct oxr_swapchain *, quad->subImage.swapchain);

	if (sc == NULL) {
		return oxr_error(log, XR_ERROR_LAYER_INVALID,
		                 "(frameEndInfo->layers[%u]->subImage."
		                 "swapchain) swapchain is NULL!",
		                 layer_index);
	}

	XrResult ret = verify_space(log, layer_index, quad->space);
	if (ret != XR_SUCCESS) {
		return ret;
	}

	if (!math_quat_validate((struct xrt_quat *)&quad->pose.orientation)) {
		XrQuaternionf *q = &quad->pose.orientation;
		return oxr_error(log, XR_ERROR_POSE_INVALID,
		                 "(frameEndInfo->layers[%u]->pose.orientation "
		                 "== {%f %f %f %f}) is not a valid quat",
		                 layer_index, q->x, q->y, q->z, q->w);
	}

	if (!math_vec3_validate((struct xrt_vec3 *)&quad->pose.position)) {
		XrVector3f *p = &quad->pose.position;
		return oxr_error(log, XR_ERROR_POSE_INVALID,
		                 "(frameEndInfo->layers[%u]->pose.position "
		                 "== {%f %f %f}) is not valid",
		                 layer_index, p->x, p->y, p->z);
	}

#if 0
	if (quad->subImage.imageArrayIndex > 0 &&
	    sc->swapchain->array_size <= quad->subImage.imageArrayIndex) {
		return oxr_error(log, XR_ERROR_VALIDATION_FAILURE,
		                 "Invalid swapchain array index for layer %u.",
		                 layer_index);
	}
#endif

	if (sc->released_index == -1) {
		return oxr_error(log, XR_ERROR_LAYER_INVALID,
		                 "(frameEndInfo->layers[%u]->subImage."
		                 "swapchain) swapchain has not been released!",
		                 layer_index);
	}

	if (sc->released_index >= (int)sc->swapchain->num_images) {
		return oxr_error(
		    log, XR_ERROR_RUNTIME_FAILURE,
		    "(frameEndInfo->layers[%u]->subImage.swapchain) internal "
		    "image index out of bounds",
		    layer_index);
	}

	if (quad->subImage.imageRect.offset.x < 0 ||
	    quad->subImage.imageRect.offset.y < 0) {
		return oxr_error(log, XR_ERROR_SWAPCHAIN_RECT_INVALID,
		                 "imageRect offset is negative for layer %u.",
		                 layer_index);
	}

	if (quad->subImage.imageRect.offset.x >= 1 ||
	    quad->subImage.imageRect.offset.y >= 1) {
		return oxr_error(log, XR_ERROR_SWAPCHAIN_RECT_INVALID,
		                 "imageRect offset out of bounds for layer %u.",
		                 layer_index);
	}

	return XR_SUCCESS;
}

static XrResult
verify_projection_layer(struct xrt_compositor *xc,
                        struct oxr_logger *log,
                        uint32_t layer_index,
                        XrCompositionLayerProjection *proj,
                        struct xrt_device *head,
                        uint64_t timestamp)
{
	XrResult ret = verify_space(log, layer_index, proj->space);
	if (ret != XR_SUCCESS) {
		return ret;
	}

	if (proj->viewCount != 2) {
		return oxr_error(log, XR_ERROR_VALIDATION_FAILURE,
		                 "(frameEndInfo->layers[%u]->viewCount == %u)"
		                 " must be 2",
		                 layer_index, proj->viewCount);
	}

	// Check for valid swapchain states.
	for (uint32_t i = 0; i < proj->viewCount; i++) {
		//! @todo More validation?
		if (!math_quat_validate(
		        (struct xrt_quat *)&proj->views[i].pose.orientation)) {
			const XrQuaternionf *q =
			    &proj->views[i].pose.orientation;
			return oxr_error(
			    log, XR_ERROR_POSE_INVALID,
			    "(frameEndInfo->layers[%u]->views[%i]->pose."
			    "orientation == {%f %f %f %f}) is not a valid quat",
			    layer_index, i, q->x, q->y, q->z, q->w);
		}

		if (!math_vec3_validate(
		        (struct xrt_vec3 *)&proj->views[i].pose.position)) {
			const XrVector3f *p = &proj->views[i].pose.position;
			return oxr_error(
			    log, XR_ERROR_POSE_INVALID,
			    "(frameEndInfo->layers[%u]->views[%i]->pose."
			    "position == {%f %f %f}) is not valid",
			    layer_index, i, p->x, p->y, p->z);
		}

		struct oxr_swapchain *sc = XRT_CAST_OXR_HANDLE_TO_PTR(
		    struct oxr_swapchain *, proj->views[i].subImage.swapchain);

		if (sc->released_index == -1) {
			return oxr_error(
			    log, XR_ERROR_LAYER_INVALID,
			    "(frameEndInfo->layers[%u]->views[%i].subImage."
			    "swapchain) swapchain has not been released",
			    layer_index, i);
		}

		if (sc->released_index >= (int)sc->swapchain->num_images) {
			return oxr_error(
			    log, XR_ERROR_RUNTIME_FAILURE,
			    "(frameEndInfo->layers[%u]->views[%i].subImage."
			    "swapchain) internal image index out of bounds",
			    layer_index, i);
		}
	}

	return XR_SUCCESS;
}

static void
submit_quad_layer(struct xrt_compositor *xc,
                  struct oxr_logger *log,
                  XrCompositionLayerQuad *quad,
                  struct xrt_device *head,
                  struct xrt_pose *inv_offset,
                  uint64_t timestamp)
{
	struct oxr_swapchain *sc = XRT_CAST_OXR_HANDLE_TO_PTR(
	    struct oxr_swapchain *, quad->subImage.swapchain);

	struct xrt_pose pose;
	math_pose_transform(inv_offset, (struct xrt_pose *)&quad->pose, &pose);

	xrt_comp_layer_quad(
	    xc, timestamp, head, XRT_INPUT_GENERIC_HEAD_POSE, quad->layerFlags,
	    (enum xrt_layer_eye_visibility)quad->eyeVisibility, sc->swapchain,
	    sc->released_index, (struct xrt_rect *)&quad->subImage.imageRect,
	    quad->subImage.imageArrayIndex, &pose,
	    (struct xrt_vec2 *)&quad->size, false);
}

static void
submit_projection_layer(struct xrt_compositor *xc,
                        struct oxr_logger *log,
                        XrCompositionLayerProjection *proj,
                        struct xrt_device *head,
                        struct xrt_pose *inv_offset,
                        uint64_t timestamp)
{
	enum xrt_layer_composition_flags flags = 0;
	struct oxr_swapchain *scs[2];

	uint32_t num_chains = ARRAY_SIZE(scs);

	for (uint32_t i = 0; i < num_chains; i++) {
		scs[i] = XRT_CAST_OXR_HANDLE_TO_PTR(
		    struct oxr_swapchain *, proj->views[i].subImage.swapchain);
	}

	struct xrt_pose pose[2];
	math_pose_transform(inv_offset, (struct xrt_pose *)&proj->views[0].pose,
	                    &pose[0]);
	math_pose_transform(inv_offset, (struct xrt_pose *)&proj->views[1].pose,
	                    &pose[1]);

	xrt_comp_layer_stereo_projection(
	    xc, timestamp, head, XRT_INPUT_GENERIC_HEAD_POSE, flags,
	    scs[0]->swapchain, // Left
	    scs[0]->released_index,
	    (struct xrt_rect *)&proj->views[0].subImage.imageRect,
	    proj->views[0].subImage.imageArrayIndex,
	    (struct xrt_fov *)&proj->views[0].fov, &pose[0],
	    scs[1]->swapchain, // Right
	    scs[1]->released_index,
	    (struct xrt_rect *)&proj->views[1].subImage.imageRect,
	    proj->views[1].subImage.imageArrayIndex,
	    (struct xrt_fov *)&proj->views[1].fov, &pose[1], false);
}

XrResult
oxr_session_frame_end(struct oxr_logger *log,
                      struct oxr_session *sess,
                      const XrFrameEndInfo *frameEndInfo)
{
	/*
	 * Session state and call order.
	 */

	if (!is_running(sess->state)) {
		return oxr_error(log, XR_ERROR_SESSION_NOT_RUNNING,
		                 " session is not running");
	}
	if (!sess->frame_started) {
		return oxr_error(log, XR_ERROR_CALL_ORDER_INVALID,
		                 " frame not begun with xrBeginFrame");
	}

	if (frameEndInfo->displayTime <= 0) {
		return oxr_error(
		    log, XR_ERROR_TIME_INVALID,
		    "(frameEndInfo->displayTime == %" PRIi64
		    ") zero or a negative value is not a valid XrTime",
		    frameEndInfo->displayTime);
	}

	struct xrt_compositor *xc = sess->compositor;

	/*
	 * early out for headless sessions.
	 */
	if (xc == NULL) {
		sess->frame_started = false;

		return oxr_session_success_result(sess);
	}


	/*
	 * Blend mode.
	 * XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED must always be reported,
	 * even with 0 layers.
	 */

	enum xrt_blend_mode blend_mode =
	    oxr_blend_mode_to_xrt(frameEndInfo->environmentBlendMode);

	if (blend_mode == 0) {
		return oxr_error(log, XR_ERROR_VALIDATION_FAILURE,
		                 "(frameEndInfo->environmentBlendMode == "
		                 "0x%08x) unknown environment blend mode",
		                 frameEndInfo->environmentBlendMode);
	}

	if ((blend_mode & sess->sys->head->hmd->blend_mode) == 0) {
		//! @todo Make integer print to string.
		return oxr_error(log,
		                 XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED,
		                 "(frameEndInfo->environmentBlendMode == %u) "
		                 "is not supported",
		                 frameEndInfo->environmentBlendMode);
	}

	/*
	 * Early out for discarded frame if layer count is 0.
	 */
	if (frameEndInfo->layerCount == 0) {
		xrt_comp_discard_frame(xc);
		sess->frame_started = false;

		return oxr_session_success_result(sess);
	}

	/*
	 * Layers.
	 */

	if (frameEndInfo->layers == NULL) {
		return oxr_error(log, XR_ERROR_LAYER_INVALID,
		                 "(frameEndInfo->layers == NULL)");
	}

	for (uint32_t i = 0; i < frameEndInfo->layerCount; i++) {
		const XrCompositionLayerBaseHeader *layer =
		    frameEndInfo->layers[i];
		if (layer == NULL) {
			return oxr_error(log, XR_ERROR_LAYER_INVALID,
			                 "(frameEndInfo->layers[%u] == NULL) "
			                 "layer can not be null",
			                 i);
		}

		XrResult res;

		switch (layer->type) {
		case XR_TYPE_COMPOSITION_LAYER_PROJECTION:
			res = verify_projection_layer(
			    xc, log, i, (XrCompositionLayerProjection *)layer,
			    sess->sys->head, frameEndInfo->displayTime);
			break;
		case XR_TYPE_COMPOSITION_LAYER_QUAD:
			res = verify_quad_layer(
			    xc, log, i, (XrCompositionLayerQuad *)layer,
			    sess->sys->head, frameEndInfo->displayTime);
			break;
		default:
			return oxr_error(log, XR_ERROR_LAYER_INVALID,
			                 "(frameEndInfo->layers[%u]->type) "
			                 "layer type not supported",
			                 i);
		}

		if (res != XR_SUCCESS) {
			return res;
		}
	}


	/*
	 * Done verifying.
	 */


	struct xrt_pose inv_offset = {0};
	math_pose_invert(&sess->sys->head->tracking_origin->offset,
	                 &inv_offset);

	xrt_comp_layer_begin(xc, blend_mode);

	for (uint32_t i = 0; i < frameEndInfo->layerCount; i++) {
		const XrCompositionLayerBaseHeader *layer =
		    frameEndInfo->layers[i];
		assert(layer != NULL);

		switch (layer->type) {
		case XR_TYPE_COMPOSITION_LAYER_PROJECTION:
			submit_projection_layer(
			    xc, log, (XrCompositionLayerProjection *)layer,
			    sess->sys->head, &inv_offset,
			    frameEndInfo->displayTime);
			break;
		case XR_TYPE_COMPOSITION_LAYER_QUAD:
			submit_quad_layer(xc, log,
			                  (XrCompositionLayerQuad *)layer,
			                  sess->sys->head, &inv_offset,
			                  frameEndInfo->displayTime);
			break;
		default: assert(false && "invalid layer type");
		}
	}

	xrt_comp_layer_commit(xc);

	sess->frame_started = false;

	return oxr_session_success_result(sess);
}

static XrResult
oxr_session_destroy(struct oxr_logger *log, struct oxr_handle_base *hb)
{
	struct oxr_session *sess = (struct oxr_session *)hb;

	XrResult ret = oxr_event_remove_session_events(log, sess);

	// Does a null-ptr check.
	xrt_comp_destroy(&sess->compositor);

	u_hashmap_int_destroy(&sess->act_sets);
	u_hashmap_int_destroy(&sess->sources);

	free(sess);

	return ret;
}

#define OXR_SESSION_ALLOCATE(LOG, SYS, OUT)                                    \
	do {                                                                   \
		OXR_ALLOCATE_HANDLE_OR_RETURN(LOG, OUT, OXR_XR_DEBUG_SESSION,  \
		                              oxr_session_destroy,             \
		                              &(SYS)->inst->handle);           \
		(OUT)->sys = (SYS);                                            \
	} while (0)

/* Just the allocation and populate part, so we can use early-returns to
 * simplify code flow and avoid weird if/else */
static XrResult
oxr_session_create_impl(struct oxr_logger *log,
                        struct oxr_system *sys,
                        const XrSessionCreateInfo *createInfo,
                        struct oxr_session **out_session)
{
#if defined(XR_USE_PLATFORM_XLIB) && defined(XR_USE_GRAPHICS_API_OPENGL)
	XrGraphicsBindingOpenGLXlibKHR const *opengl_xlib =
	    OXR_GET_INPUT_FROM_CHAIN(createInfo,
	                             XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR,
	                             XrGraphicsBindingOpenGLXlibKHR);
	if (opengl_xlib != NULL) {
		if (!sys->gotten_requirements) {
			return oxr_error(
			    log, XR_ERROR_VALIDATION_FAILURE,
			    "Has not called "
			    "xrGetOpenGL[ES]GraphicsRequirementsKHR");
		}

		OXR_SESSION_ALLOCATE(log, sys, *out_session);
		return oxr_session_populate_gl_xlib(log, sys, opengl_xlib,
		                                    *out_session);
	}
#endif

#ifdef XR_USE_GRAPHICS_API_VULKAN
	XrGraphicsBindingVulkanKHR const *vulkan = OXR_GET_INPUT_FROM_CHAIN(
	    createInfo, XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR,
	    XrGraphicsBindingVulkanKHR);
	if (vulkan != NULL) {
		if (!sys->gotten_requirements) {
			return oxr_error(log, XR_ERROR_VALIDATION_FAILURE,
			                 "Has not called "
			                 "xrGetVulkanGraphicsRequirementsKHR");
		}

		OXR_SESSION_ALLOCATE(log, sys, *out_session);
		return oxr_session_populate_vk(log, sys, vulkan, *out_session);
	}
#endif

#ifdef XR_USE_PLATFORM_EGL
	XrGraphicsBindingEGLMNDX const *egl = OXR_GET_INPUT_FROM_CHAIN(
	    createInfo, XR_TYPE_GRAPHICS_BINDING_EGL_MNDX,
	    XrGraphicsBindingEGLMNDX);
	if (egl != NULL) {
		if (!sys->gotten_requirements) {
			return oxr_error(
			    log, XR_ERROR_VALIDATION_FAILURE,
			    "Has not called "
			    "xrGetOpenGL[ES]GraphicsRequirementsKHR");
		}

		OXR_SESSION_ALLOCATE(log, sys, *out_session);
		return oxr_session_populate_egl(log, sys, egl, *out_session);
	}
#endif

	/*
	 * Add any new graphics binding structs here - before the headless
	 * check. (order for non-headless checks not specified in standard.)
	 * Any new addition will also need to be added to
	 * oxr_verify_XrSessionCreateInfo and have its own associated verify
	 * function added.
	 */

	if (sys->inst->extensions.MND_headless) {
		OXR_SESSION_ALLOCATE(log, sys, *out_session);
		(*out_session)->compositor = NULL;
		(*out_session)->create_swapchain = NULL;
		return XR_SUCCESS;
	}
	return oxr_error(log, XR_ERROR_VALIDATION_FAILURE,
	                 "(createInfo->next->type)");
}

XrResult
oxr_session_create(struct oxr_logger *log,
                   struct oxr_system *sys,
                   const XrSessionCreateInfo *createInfo,
                   struct oxr_session **out_session)
{
	struct oxr_session *sess = NULL;

	/* Try allocating and populating. */
	XrResult ret = oxr_session_create_impl(log, sys, createInfo, &sess);
	if (ret != XR_SUCCESS) {
		if (sess != NULL) {
			/* clean up allocation first */
			XrResult cleanup_result =
			    oxr_handle_destroy(log, &sess->handle);
			assert(cleanup_result == XR_SUCCESS);
			(void)cleanup_result;
		}
		return ret;
	}

	sess->ipd_meters = debug_get_num_option_ipd() / 1000.0f;
	sess->static_prediction_s =
	    debug_get_num_option_prediction_ms() / 1000.0f;

	oxr_event_push_XrEventDataSessionStateChanged(log, sess,
	                                              XR_SESSION_STATE_IDLE, 0);
	oxr_event_push_XrEventDataSessionStateChanged(
	    log, sess, XR_SESSION_STATE_READY, 0);
	sess->state = XR_SESSION_STATE_READY;

	u_hashmap_int_create(&sess->act_sets);
	u_hashmap_int_create(&sess->sources);

	*out_session = sess;

	return ret;
}
