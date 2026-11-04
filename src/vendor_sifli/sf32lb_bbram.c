/****************************************************************************
 * vendor/sifli/chips/sf32lb52/sf32lb_bbram.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stddef.h>

#include "sf32lb_bbram.h"

#include <bf0_hal_hlp.h>

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sf32lb_bbram_read(uint8_t idx, uint32_t *value)
{
  if (value == NULL || idx > SF32LB_BBRAM_MAX_INDEX)
    {
      return -EINVAL;
    }

  *value = HAL_Get_backup(idx);
  return 0;
}

int sf32lb_bbram_write(uint8_t idx, uint32_t value)
{
  if (idx > SF32LB_BBRAM_MAX_INDEX)
    {
      return -EINVAL;
    }

  HAL_Set_backup(idx, value);
  return 0;
}
