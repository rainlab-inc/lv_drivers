/**
 * @file glfw_gles.h
 *
 */

#ifndef GLFW_GLES_H
#define GLFW_GLES_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#ifndef LV_DRV_NO_CONF
#ifdef LV_CONF_INCLUDE_SIMPLE
#include "lv_drv_conf.h"
#else
#include "../../lv_drv_conf.h"
#endif
#endif

#if USE_GLFW_GLES

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 * GLOBAL PROTOTYPES
 **********************/

void glfw_gles_init(void);

void glfw_gles_disp_draw_buf_init(lv_disp_draw_buf_t *draw_buf);

void glfw_gles_disp_drv_init(lv_disp_drv_t *driver, lv_disp_draw_buf_t *draw_buf);

void glfw_gles_display_flush(lv_disp_drv_t * disp_drv, const lv_area_t * area, lv_color_t * color_p);

void glfw_gles_mouse_read(lv_indev_drv_t * indev_drv, lv_indev_data_t * data);

void glfw_gles_touch_read(lv_indev_drv_t * indev_drv, lv_indev_data_t * data);

/**********************
 *      MACROS
 **********************/

#endif /* USE_GLFW_GLES */

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*GLFW_GLES_H*/
