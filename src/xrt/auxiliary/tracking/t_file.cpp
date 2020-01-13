// Copyright 2019-2020, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Handling of files and calibration data.
 * @author Pete Black <pblack@collabora.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Ryan Pavlik <ryan.pavlik@collabora.com>
 * @ingroup aux_tracking
 */

#include "tracking/t_calibration_opencv.hpp"
#include "util/u_misc.h"


/*
 *
 * Pre-declar functions.
 *
 */

static int
mkpath(char *path);

static bool
read_cv_mat(FILE *f, cv::Mat *m, const char *name);

static bool
write_cv_mat(FILE *f, cv::Mat *m);


/*
 *
 * Free functions.
 *
 */

extern "C" void
t_stereo_camera_calibration_free(struct t_stereo_camera_calibration **data_ptr)
{
	free(*data_ptr);
	*data_ptr = NULL;
}


/*
 *
 * Refine and create functions.
 *
 */

RemapPair
calibration_get_undistort_map(t_camera_calibration &calib,
                              cv::InputArray rectify_transform_optional,
                              cv::Mat new_camera_matrix_optional)
{
	RemapPair ret;
	CameraCalibrationWrapper wrap(calib);
	if (new_camera_matrix_optional.empty()) {
		new_camera_matrix_optional = wrap.intrinsics_mat;
	}

	//! @todo Scale Our intrinsics if the frame size we request
	//              calibration for does not match what was saved
	cv::Size image_size(calib.image_size_pixels.w,
	                    calib.image_size_pixels.h);

	if (calib.use_fisheye) {
		cv::fisheye::initUndistortRectifyMap(
		    wrap.intrinsics_mat,         // cameraMatrix
		    wrap.distortion_fisheye_mat, // distCoeffs
		    rectify_transform_optional,  // R
		    new_camera_matrix_optional,  // newCameraMatrix
		    image_size,                  // size
		    CV_32FC1,                    // m1type
		    ret.remap_x,                 // map1
		    ret.remap_y);                // map2
	} else {
		cv::initUndistortRectifyMap(
		    wrap.intrinsics_mat,        // cameraMatrix
		    wrap.distortion_mat,        // distCoeffs
		    rectify_transform_optional, // R
		    new_camera_matrix_optional, // newCameraMatrix
		    image_size,                 // size
		    CV_32FC1,                   // m1type
		    ret.remap_x,                // map1
		    ret.remap_y);               // map2
	}

	return ret;
}

StereoRectificationMaps::StereoRectificationMaps(
    t_stereo_camera_calibration &data)
{
	assert(data.l_calibration.image_size_pixels.w ==
	       data.r_calibration.image_size_pixels.w);
	assert(data.l_calibration.image_size_pixels.h ==
	       data.r_calibration.image_size_pixels.h);

	assert(data.l_calibration.use_fisheye ==
	       data.r_calibration.use_fisheye);

	cv::Size image_size(data.l_calibration.image_size_pixels.w,
	                    data.l_calibration.image_size_pixels.h);
	StereoCameraCalibrationWrapper wrapped(data);

	/*
	 * Generate our rectification maps
	 *
	 * Here cv::noArray() means zero distortion.
	 */
	if (data.l_calibration.use_fisheye) {
		//! @todo for some reason this looks weird?
		cv::fisheye::stereoRectify(
		    wrapped.l_calibration.intrinsics_mat,         // K1
		    wrapped.l_calibration.distortion_fisheye_mat, // D1
		    /* cv::noArray(), */                          // D1
		    wrapped.r_calibration.intrinsics_mat,         // K2
		    wrapped.r_calibration.distortion_fisheye_mat, // D2
		    /* cv::noArray(), */                          // D2
		    image_size,                                   // imageSize
		    wrapped.camera_rotation_mat,                  // R
		    wrapped.camera_translation_mat,               // tvec
		    l_rotation_mat,                               // R1
		    r_rotation_mat,                               // R2
		    l_projection_mat,                             // P1
		    r_projection_mat,                             // P2
		    disparity_to_depth_mat,                       // Q
		    cv::CALIB_ZERO_DISPARITY                      // flags
		);
	} else {
		cv::stereoRectify(
		    wrapped.l_calibration.intrinsics_mat, // cameraMatrix1
		    /* cv::noArray(), */                  // distCoeffs1
		    wrapped.l_calibration.distortion_mat, // distCoeffs1
		    wrapped.r_calibration.intrinsics_mat, // cameraMatrix2
		    /* cv::noArray(), */                  // distCoeffs2
		    wrapped.r_calibration.distortion_mat, // distCoeffs2
		    image_size,                           // imageSize
		    wrapped.camera_rotation_mat,          // R
		    wrapped.camera_translation_mat,       // T
		    l_rotation_mat,                       // R1
		    r_rotation_mat,                       // R2
		    l_projection_mat,                     // P1
		    r_projection_mat,                     // P2
		    disparity_to_depth_mat,               // Q
		    cv::CALIB_ZERO_DISPARITY,             // flags
		    -1,                                   // alpha
		    image_size,                           // newImageSize
		    NULL,                                 // validPixROI1
		    NULL);                                // validPixROI2
	}

	l_rectify = calibration_get_undistort_map(
	    data.l_calibration, l_rotation_mat, l_projection_mat);
	r_rectify = calibration_get_undistort_map(
	    data.r_calibration, r_rotation_mat, r_projection_mat);
}

/*
 *
 * Load functions.
 *
 */

extern "C" bool
t_stereo_camera_calibration_load_v1(
    FILE *calib_file, struct t_stereo_camera_calibration **out_data)
{
	t_stereo_camera_calibration &raw =
	    *U_TYPED_CALLOC(t_stereo_camera_calibration);
	StereoCameraCalibrationWrapper wrapped(raw);

	// Dummy matrix
	cv::Mat dummy;

	// Read our calibration from this file
	// clang-format off
	read_cv_mat(calib_file, &wrapped.l_calibration.intrinsics_mat, "l_intrinsics"); // 3 x 3
	read_cv_mat(calib_file, &wrapped.r_calibration.intrinsics_mat, "r_intrinsics"); // 3 x 3
	read_cv_mat(calib_file, &wrapped.l_calibration.distortion_mat, "l_distortion"); // 1 x 5
	read_cv_mat(calib_file, &wrapped.r_calibration.distortion_mat, "r_distortion"); // 1 x 5
	read_cv_mat(calib_file, &wrapped.l_calibration.distortion_fisheye_mat, "l_distortion_fisheye"); // 4 x 1
	read_cv_mat(calib_file, &wrapped.r_calibration.distortion_fisheye_mat, "r_distortion_fisheye"); // 4 x 1
	read_cv_mat(calib_file, &dummy, "l_rotation"); // 3 x 3
	read_cv_mat(calib_file, &dummy, "r_rotation"); // 3 x 3
	read_cv_mat(calib_file, &dummy, "l_translation"); // empty
	read_cv_mat(calib_file, &dummy, "r_translation"); // empty
	read_cv_mat(calib_file, &dummy, "l_projection"); // 3 x 4
	read_cv_mat(calib_file, &dummy, "r_projection"); // 3 x 4
	read_cv_mat(calib_file, &dummy, "disparity_to_depth");  // 4 x 4
	cv::Mat mat_image_size = {};
	read_cv_mat(calib_file, &mat_image_size, "mat_image_size");

	wrapped.l_calibration.image_size_pixels.w = uint32_t(mat_image_size.at<float>(0, 0));
	wrapped.l_calibration.image_size_pixels.h = uint32_t(mat_image_size.at<float>(0, 1));
	wrapped.r_calibration.image_size_pixels = wrapped.l_calibration.image_size_pixels;

	cv::Mat mat_new_image_size = mat_image_size.clone();
	if (read_cv_mat(calib_file, &mat_new_image_size, "mat_new_image_size")) {
		// do nothing particular here.
	}

	if (!read_cv_mat(calib_file, &wrapped.camera_translation_mat, "translation")) {
		fprintf(stderr, "\tRe-run calibration!\n");
	}
	if (!read_cv_mat(calib_file, &wrapped.camera_rotation_mat, "rotation")) {
		fprintf(stderr, "\tRe-run calibration!\n");
	}
	if (!read_cv_mat(calib_file, &wrapped.camera_essential_mat, "essential")) {
		fprintf(stderr, "\tRe-run calibration!\n");
	}
	if (!read_cv_mat(calib_file, &wrapped.camera_fundamental_mat, "fundamental")) {
		fprintf(stderr, "\tRe-run calibration!\n");
	}

	cv::Mat mat_use_fisheye = {};
	if (!read_cv_mat(calib_file, &mat_use_fisheye, "use_fisheye")) {
		wrapped.l_calibration.use_fisheye = false;
		fprintf(stderr, "\tRe-run calibration! (Assuming not fisheye)\n");
	} else {
		wrapped.l_calibration.use_fisheye = mat_use_fisheye.at<float>(0, 0) != 0.0;
	}
	wrapped.r_calibration.use_fisheye = wrapped.l_calibration.use_fisheye;
	// clang-format on

	if (wrapped.camera_translation_mat.size() == cv::Size(3, 1)) {
		//! @todo don't understand this code - looks like it's just
		//! self-assigning
		fprintf(stderr,
		        "Readjusting translation, re-run calibration.\n");
		raw.camera_translation[0] =
		    wrapped.camera_translation_mat.at<double>(0, 0);
		raw.camera_translation[1] =
		    wrapped.camera_translation_mat.at<double>(0, 1);
		raw.camera_translation[2] =
		    wrapped.camera_translation_mat.at<double>(0, 2);
		wrapped.camera_translation_mat =
		    cv::Mat(3, 1, CV_64F, &raw.camera_translation[0]);
	}

	assert(wrapped.isDataStorageValid());
	*out_data = &raw;

	return true;
}


/*
 *
 * Save functions.
 *
 */

extern "C" bool
t_file_save_raw_data(FILE *calib_file, struct t_stereo_camera_calibration *data)
{
	StereoCameraCalibrationWrapper wrapped(*data);
	// Dummy matrix
	cv::Mat dummy;


	write_cv_mat(calib_file, &wrapped.l_calibration.intrinsics_mat);
	write_cv_mat(calib_file, &wrapped.r_calibration.intrinsics_mat);
	write_cv_mat(calib_file, &wrapped.l_calibration.distortion_mat);
	write_cv_mat(calib_file, &wrapped.r_calibration.distortion_mat);
	write_cv_mat(calib_file, &wrapped.l_calibration.distortion_fisheye_mat);
	write_cv_mat(calib_file, &wrapped.r_calibration.distortion_fisheye_mat);
	write_cv_mat(calib_file, &dummy); // l_rotation_mat
	write_cv_mat(calib_file, &dummy); // r_rotation_mat
	write_cv_mat(calib_file, &dummy); // l_translation
	write_cv_mat(calib_file, &dummy); // r_translation
	write_cv_mat(calib_file, &dummy); // l_projection_mat
	write_cv_mat(calib_file, &dummy); // r_projection_mat
	write_cv_mat(calib_file, &dummy); // disparity_to_depth_mat

	cv::Mat mat_image_size;
	mat_image_size.create(1, 2, CV_32F);
	mat_image_size.at<float>(0, 0) =
	    wrapped.l_calibration.image_size_pixels.w;
	mat_image_size.at<float>(0, 1) =
	    wrapped.l_calibration.image_size_pixels.h;
	write_cv_mat(calib_file, &mat_image_size);

	// "new" image size - we actually leave up to the caller now
	write_cv_mat(calib_file, &mat_image_size);

	write_cv_mat(calib_file, &wrapped.camera_translation_mat);
	write_cv_mat(calib_file, &wrapped.camera_rotation_mat);
	write_cv_mat(calib_file, &wrapped.camera_essential_mat);
	write_cv_mat(calib_file, &wrapped.camera_fundamental_mat);

	cv::Mat mat_use_fisheye;
	mat_use_fisheye.create(1, 1, CV_32F);
	mat_use_fisheye.at<float>(0, 0) = wrapped.l_calibration.use_fisheye;
	write_cv_mat(calib_file, &mat_use_fisheye);

	return true;
}


/*
 *
 * Hack functions.
 *
 */

extern "C" bool
t_stereo_camera_calibration_load_v1_hack(
    struct t_stereo_camera_calibration **out_data)
{
	const char *configuration_filename = "PS4_EYE";

	char path_string[256]; //! @todo 256 maybe not enough
	//! @todo Use multiple env vars?
	char *config_path = secure_getenv("HOME");
	snprintf(path_string, 256, "%s/.config/monado/%s.calibration",
	         config_path, configuration_filename); //! @todo Hardcoded 256

	FILE *calib_file = fopen(path_string, "rb");
	if (calib_file == NULL) {
		return false;
	}

	bool ret = t_stereo_camera_calibration_load_v1(calib_file, out_data);

	fclose(calib_file);

	return ret;
}

extern "C" bool
t_file_save_raw_data_hack(struct t_stereo_camera_calibration *data)
{
	char path_string[PATH_MAX];
	char file_string[PATH_MAX];
	// TODO: centralise this - use multiple env vars?
	char *config_path = secure_getenv("HOME");
	snprintf(path_string, PATH_MAX, "%s/.config/monado", config_path);
	snprintf(file_string, PATH_MAX, "%s/.config/monado/%s.calibration",
	         config_path, "PS4_EYE");
	FILE *calib_file = fopen(file_string, "wb");
	if (!calib_file) {
		// try creating it
		mkpath(path_string);
	}
	calib_file = fopen(file_string, "wb");
	if (!calib_file) {
		printf(
		    "ERROR. could not create calibration file "
		    "%s\n",
		    file_string);
		return false;
	}

	t_file_save_raw_data(calib_file, data);

	fclose(calib_file);

	return true;
}


/*
 *
 * Helpers
 *
 */

//! @todo Move this as it is a generic helper
static int
mkpath(char *path)
{
	char tmp[PATH_MAX]; //!< @todo PATH_MAX probably not strictly correct
	char *p = nullptr;
	size_t len;

	snprintf(tmp, sizeof(tmp), "%s", path);
	len = strlen(tmp) - 1;
	if (tmp[len] == '/') {
		tmp[len] = 0;
	}

	for (p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = 0;
			if (mkdir(tmp, S_IRWXU) < 0 && errno != EEXIST)
				return -1;
			*p = '/';
		}
	}

	if (mkdir(tmp, S_IRWXU) < 0 && errno != EEXIST) {
		return -1;
	}

	return 0;
}

static bool
write_cv_mat(FILE *f, cv::Mat *m)
{
	uint32_t header[3];
	header[0] = static_cast<uint32_t>(m->elemSize());
	header[1] = static_cast<uint32_t>(m->rows);
	header[2] = static_cast<uint32_t>(m->cols);
	fwrite(static_cast<void *>(header), sizeof(uint32_t), 3, f);
	fwrite(static_cast<void *>(m->data), header[0], header[1] * header[2],
	       f);
	return true;
}

static bool
read_cv_mat(FILE *f, cv::Mat *m, const char *name)
{
	uint32_t header[3] = {};
	size_t read = 0;

	read = fread(static_cast<void *>(header), sizeof(uint32_t), 3, f);
	if (read != 3) {
		printf("Failed to read mat header: '%i' '%s'\n", (int)read,
		       name);
		return false;
	}

	if (header[1] == 0 && header[2] == 0) {
		return true;
	}

	//! @todo We may have written things other than CV_32F and CV_64F.
	if (header[0] == 4) {
		m->create(static_cast<int>(header[1]),
		          static_cast<int>(header[2]), CV_32F);
	} else {
		m->create(static_cast<int>(header[1]),
		          static_cast<int>(header[2]), CV_64F);
	}
	read = fread(static_cast<void *>(m->data), header[0],
	             header[1] * header[2], f);
	if (read != (header[1] * header[2])) {
		printf("Failed to read mat body: '%i' '%s'\n", (int)read, name);
		return false;
	}

	return true;
}
