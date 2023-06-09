/*
 * Copyright (C) 2017 MediaTek Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See http://www.gnu.org/licenses/gpl-2.0.html for more details.
 */

#include <linux/delay.h>
#include <linux/string.h>

#include "kd_camera_typedef.h"
#include "kd_camera_feature.h"

#include "imgsensor_sensor.h"
#include "imgsensor_hw.h"

enum IMGSENSOR_RETURN imgsensor_hw_init(struct IMGSENSOR_HW *phw)
{
	struct IMGSENSOR_HW_SENSOR_POWER      *psensor_pwr;
	struct IMGSENSOR_HW_CFG               *pcust_pwr_cfg;
	struct IMGSENSOR_HW_CUSTOM_POWER_INFO *ppwr_info;
	int i, j;
	char str_prop_name[LENGTH_FOR_SNPRINTF];
	struct device_node *of_node
		= of_find_compatible_node(NULL, NULL, "mediatek,imgsensor");

	mutex_init(&phw->common.pinctrl_mutex);

	for (i = 0; i < IMGSENSOR_HW_ID_MAX_NUM; i++) {
		if (hw_open[i] != NULL)
			(hw_open[i]) (&phw->pdev[i]);

		if (phw->pdev[i]->init != NULL)
			(phw->pdev[i]->init)(
				phw->pdev[i]->pinstance, &phw->common);
	}

	for (i = 0; i < IMGSENSOR_SENSOR_IDX_MAX_NUM; i++) {
		psensor_pwr = &phw->sensor_pwr[i];

		pcust_pwr_cfg = imgsensor_custom_config;
		while (pcust_pwr_cfg->sensor_idx != i &&
		       pcust_pwr_cfg->sensor_idx != IMGSENSOR_SENSOR_IDX_NONE)
			pcust_pwr_cfg++;

		if (pcust_pwr_cfg->sensor_idx == IMGSENSOR_SENSOR_IDX_NONE)
			continue;

		ppwr_info = pcust_pwr_cfg->pwr_info;
		while (ppwr_info->pin != IMGSENSOR_HW_PIN_NONE) {
			for (j = 0;
				j < IMGSENSOR_HW_ID_MAX_NUM &&
					ppwr_info->id != phw->pdev[j]->id;
				j++) {
			}

			psensor_pwr->id[ppwr_info->pin] = j;
			ppwr_info++;
		}
	}

	for (i = 0; i < IMGSENSOR_SENSOR_IDX_MAX_NUM; i++) {
		memset(str_prop_name, 0, sizeof(str_prop_name));
		snprintf(str_prop_name,
					sizeof(str_prop_name),
					"cam%d_%s",
					i,
					"enable_sensor");
		if (of_property_read_string(
			of_node, str_prop_name,
			&phw->enable_sensor_by_index[i]) < 0) {
			PK_DBG("Property cust-sensor not defined\n");
			phw->enable_sensor_by_index[i] = NULL;
		}
	}

	return IMGSENSOR_RETURN_SUCCESS;
}

enum IMGSENSOR_RETURN imgsensor_hw_release_all(struct IMGSENSOR_HW *phw)
{
	int i;

	for (i = 0; i < IMGSENSOR_HW_ID_MAX_NUM; i++) {
		if (phw->pdev[i]->release != NULL)
			(phw->pdev[i]->release)(phw->pdev[i]->pinstance);
	}
	return IMGSENSOR_RETURN_SUCCESS;
}

static enum IMGSENSOR_RETURN imgsensor_hw_power_sequence(
		struct IMGSENSOR_HW             *phw,
		enum   IMGSENSOR_SENSOR_IDX      sensor_idx,
		enum   IMGSENSOR_HW_POWER_STATUS pwr_status,
		struct IMGSENSOR_HW_POWER_SEQ   *ppower_sequence,
		char *pcurr_idx)
{
	struct IMGSENSOR_HW_SENSOR_POWER *psensor_pwr =
					&phw->sensor_pwr[sensor_idx];
	struct IMGSENSOR_HW_POWER_SEQ    *ppwr_seq = ppower_sequence;
	struct IMGSENSOR_HW_POWER_INFO   *ppwr_info;
	struct IMGSENSOR_HW_DEVICE       *pdev;
	int                               pin_cnt = 0;

	static DEFINE_RATELIMIT_STATE(ratelimit, 1 * HZ, 30);

#ifdef CONFIG_FPGA_EARLY_PORTING  /*for FPGA*/
	if (1) {
		PK_DBG("FPGA return true for power control\n");
		return IMGSENSOR_RETURN_SUCCESS;
	}
#endif

	while (ppwr_seq < ppower_sequence + IMGSENSOR_HW_SENSOR_MAX_NUM &&
		ppwr_seq->name != NULL) {
		if (!strcmp(ppwr_seq->name, PLATFORM_POWER_SEQ_NAME)) {
			if (sensor_idx == ppwr_seq->_idx)
				break;
		} else {
			if (!strcmp(ppwr_seq->name, pcurr_idx))
				break;
		}
		ppwr_seq++;
	}

	if (ppwr_seq->name == NULL)
		return IMGSENSOR_RETURN_ERROR;

	ppwr_info = ppwr_seq->pwr_info;

	while (ppwr_info->pin != IMGSENSOR_HW_PIN_NONE &&
	       ppwr_info < ppwr_seq->pwr_info + IMGSENSOR_HW_POWER_INFO_MAX) {

		if (pwr_status == IMGSENSOR_HW_POWER_STATUS_ON) {
			if (ppwr_info->pin != IMGSENSOR_HW_PIN_UNDEF) {
				pdev =
				phw->pdev[psensor_pwr->id[ppwr_info->pin]];

				if (__ratelimit(&ratelimit))
					PK_DBG(
					"sensor_idx %d, ppwr_info->pin %d, ppwr_info->pin_state_on %d",
					sensor_idx,
					ppwr_info->pin,
					ppwr_info->pin_state_on);

				if (pdev->set != NULL)
					pdev->set(pdev->pinstance,
					sensor_idx,
				    ppwr_info->pin, ppwr_info->pin_state_on);
			}

			mdelay(ppwr_info->pin_on_delay);
		}

		ppwr_info++;
		pin_cnt++;
	}

	if (pwr_status == IMGSENSOR_HW_POWER_STATUS_OFF) {
		while (pin_cnt) {
			ppwr_info--;
			pin_cnt--;

			if (__ratelimit(&ratelimit))
				PK_DBG(
				"sensor_idx %d, ppwr_info->pin %d, ppwr_info->pin_state_off %d",
				sensor_idx,
				ppwr_info->pin,
				ppwr_info->pin_state_off);

			if (ppwr_info->pin != IMGSENSOR_HW_PIN_UNDEF) {
				pdev =
				phw->pdev[psensor_pwr->id[ppwr_info->pin]];

				if (pdev->set != NULL)
					pdev->set(pdev->pinstance,
					sensor_idx,
				ppwr_info->pin, ppwr_info->pin_state_off);
			}

			mdelay(ppwr_info->pin_on_delay);
		}
	}

	return IMGSENSOR_RETURN_SUCCESS;
}

#ifdef CONFIG_CAMERA_PROJECT_LIMA
extern void AFRegulatorCtrl(int Stage);
int mipiswitch(
		struct IMGSENSOR_HW             *phw,
		enum   IMGSENSOR_SENSOR_IDX      sensor_idx,
		enum   IMGSENSOR_HW_POWER_STATUS pwr_status)
{
	struct pinctrl_state *cam_mipi_switch_sel_h = NULL;/* for mipi switch select */
	struct pinctrl_state *cam_mipi_switch_sel_l = NULL;
	struct pinctrl *camctrl = NULL;
	int ret = 0;

	camctrl = devm_pinctrl_get(&(phw->common.pplatform_device->dev));
	cam_mipi_switch_sel_h = pinctrl_lookup_state(camctrl, "cam_mipi_switch_sel_1");
	cam_mipi_switch_sel_l = pinctrl_lookup_state(camctrl, "cam_mipi_switch_sel_0");
	if (pwr_status == IMGSENSOR_HW_POWER_STATUS_ON){
		if (IMGSENSOR_SENSOR_IDX_MAIN3 == sensor_idx) {
			pinctrl_select_state(camctrl, cam_mipi_switch_sel_h);
		} else if (IMGSENSOR_SENSOR_IDX_SUB == sensor_idx) {
			pinctrl_select_state(camctrl, cam_mipi_switch_sel_l);
		}
	}else if (pwr_status == IMGSENSOR_HW_POWER_STATUS_OFF) {
		if (IMGSENSOR_SENSOR_IDX_SUB == sensor_idx || IMGSENSOR_SENSOR_IDX_MAIN3 == sensor_idx) {
			pinctrl_select_state(camctrl, cam_mipi_switch_sel_l);
		}
	}
	return ret;
}
#endif
enum IMGSENSOR_RETURN imgsensor_hw_power(
		struct IMGSENSOR_HW *phw,
		struct IMGSENSOR_SENSOR *psensor,
		enum IMGSENSOR_HW_POWER_STATUS pwr_status)
{
	enum IMGSENSOR_SENSOR_IDX sensor_idx = psensor->inst.sensor_idx;
	char *curr_sensor_name = psensor->inst.psensor_list->name;
	char str_index[LENGTH_FOR_SNPRINTF];

	PK_DBG("sensor_idx %d, power %d curr_sensor_name %s, enable list %s\n",
		sensor_idx,
		pwr_status,
		curr_sensor_name,
		phw->enable_sensor_by_index[sensor_idx] == NULL
		? "NULL"
		: phw->enable_sensor_by_index[sensor_idx]);

	if (phw->enable_sensor_by_index[sensor_idx] &&
	!strstr(phw->enable_sensor_by_index[sensor_idx], curr_sensor_name))
		return IMGSENSOR_RETURN_ERROR;


	snprintf(str_index, sizeof(str_index), "%d", sensor_idx);
#ifdef CONFIG_CAMERA_PROJECT_LIMA
	if (IMGSENSOR_SENSOR_IDX_SUB2 == sensor_idx){
	      return IMGSENSOR_RETURN_ERROR;
	}
	if ((strcmp(curr_sensor_name,"s5k4h7yx_mipi_raw")==0 || strcmp(curr_sensor_name,"s5k4h7qt_mipi_raw")==0 ||
	      strcmp(curr_sensor_name,"ov02a10_mipi_raw")==0 || strcmp(curr_sensor_name,"ov2180_mipi_raw")==0)
	      && IMGSENSOR_SENSOR_IDX_MAIN3 == sensor_idx){
	      return IMGSENSOR_RETURN_ERROR;
	}
	if ((sensor_idx < IMGSENSOR_SENSOR_IDX_MAIN2) && strcmp(curr_sensor_name,"ov2180_mipi_raw")==0){
	      PK_DBG("sensor_idx < IDX_MAIN2\n");
	      return IMGSENSOR_RETURN_ERROR;
	}
	mipiswitch(phw, sensor_idx, pwr_status);
#endif
	imgsensor_hw_power_sequence(
			phw,
			sensor_idx,
			pwr_status,
			platform_power_sequence,
			str_index);

	imgsensor_hw_power_sequence(
			phw,
			sensor_idx,
			pwr_status, sensor_power_sequence, curr_sensor_name);
#ifdef CONFIG_CAMERA_PROJECT_LIMA
	if (strcmp(curr_sensor_name,"s5k3l6_mipi_raw")==0 || strcmp(curr_sensor_name,"s5k3l6qt_mipi_raw")==0 || strcmp(curr_sensor_name,"s5k3l6_backup_mipi_raw")==0)
	{
		PK_DBG("AFRegulatorCtrl sensor_idx %d, power %d curr_sensor_name %s\n", sensor_idx, pwr_status, curr_sensor_name);

		if (pwr_status == IMGSENSOR_HW_POWER_STATUS_ON)
		{
			AFRegulatorCtrl(0);
			AFRegulatorCtrl(1);
		}
		else
		{
			AFRegulatorCtrl(2);
		}
	}
#endif

	return IMGSENSOR_RETURN_SUCCESS;
}

enum IMGSENSOR_RETURN imgsensor_hw_dump(struct IMGSENSOR_HW *phw)
{
	int i;

	for (i = 0; i < IMGSENSOR_HW_ID_MAX_NUM; i++) {
		if (phw->pdev[i]->dump != NULL)
			(phw->pdev[i]->dump)(phw->pdev[i]->pinstance);
	}
	return IMGSENSOR_RETURN_SUCCESS;
}

