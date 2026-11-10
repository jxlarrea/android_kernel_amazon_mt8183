/******************************************************************************
 * Copyright 2020 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 *         file: theia_motion_adc.h
 *  description: Motor adc header file
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 ******************************************************************************
 */
#ifndef __THEIA_MOTION_ADC_H
#define __THEIA_MOTION_ADC_H

#include "theia_motion.h"

int theia_motion_adc_init(struct theia_motion *controller);
void theia_motion_adc_remove(struct theia_motion *controller);

#endif /* __THEIA_MOTION_ADC_H */
