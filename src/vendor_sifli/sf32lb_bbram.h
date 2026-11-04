/****************************************************************************
 * vendor/sifli/chips/sf32lb52/include/sf32lb_bbram.h
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

#ifndef __VENDOR_SIFLI_CHIPS_SF32LB52_INCLUDE_SF32LB_BBRAM_H
#define __VENDOR_SIFLI_CHIPS_SF32LB52_INCLUDE_SF32LB_BBRAM_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* BKP0R..BKP9R are the ten RTC backup registers exposed by this part. */

#define SF32LB_BBRAM_MAX_INDEX 9

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * Name: sf32lb_bbram_read
 *
 * Description:
 *   Read one 32-bit RTC backup register.
 *
 * Returned Value:
 *   Zero on success; a negated errno value on failure.
 *
 ****************************************************************************/

int sf32lb_bbram_read(uint8_t idx, uint32_t *value);

/****************************************************************************
 * Name: sf32lb_bbram_write
 *
 * Description:
 *   Write one 32-bit RTC backup register.  The caller is responsible for
 *   reading it back if it needs to know the write stuck.
 *
 * Returned Value:
 *   Zero on success; a negated errno value on failure.
 *
 ****************************************************************************/

int sf32lb_bbram_write(uint8_t idx, uint32_t value);

#ifdef __cplusplus
}
#endif

#endif /* __VENDOR_SIFLI_CHIPS_SF32LB52_INCLUDE_SF32LB_BBRAM_H */
