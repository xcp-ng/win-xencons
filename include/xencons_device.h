/* Copyright (c) Xen Project.
 * Copyright (c) Cloud Software Group, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms,
 * with or without modification, are permitted provided
 * that the following conditions are met:
 *
 * *   Redistributions of source code must retain the above
 *     copyright notice, this list of conditions and the
 *     following disclaimer.
 * *   Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the
 *     following disclaimer in the documentation and/or other
 *     materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef _XENCONS_DEVICE_H
#define _XENCONS_DEVICE_H

// {0D3EDD21-8EF9-4DFF-856C-8C68BF4FDCA3}
DEFINE_GUID(GUID_XENCONS_DEVICE,
            0xd3edd21, 0x8ef9, 0x4dff, 0x85, 0x6c, 0x8c, 0x68, 0xbf, 0x4f, 0xdc, 0xa3);

#define __IOCTL_XENCONS_BEGIN   0x800

#define IOCTL_XENCONS_GET_INSTANCE  CTL_CODE(FILE_DEVICE_UNKNOWN,       \
                                             __IOCTL_XENCONS_BEGIN + 0, \
                                             METHOD_BUFFERED,           \
                                             FILE_ANY_ACCESS)

#define IOCTL_XENCONS_GET_NAME      CTL_CODE(FILE_DEVICE_UNKNOWN,       \
                                             __IOCTL_XENCONS_BEGIN + 1, \
                                             METHOD_BUFFERED,           \
                                             FILE_ANY_ACCESS)

#define IOCTL_XENCONS_GET_PROTOCOL  CTL_CODE(FILE_DEVICE_UNKNOWN,       \
                                             __IOCTL_XENCONS_BEGIN + 2, \
                                             METHOD_BUFFERED,           \
                                             FILE_ANY_ACCESS)

#endif  // _XENCONS_DEVICE_H
