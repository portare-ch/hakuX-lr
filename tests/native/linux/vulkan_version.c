/* Copyright (C) 2026-present PortareOS */
/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "hw/xbox/hle/vk/vkdev.h"

static uint32_t loader_version;

static VKAPI_ATTR VkResult VKAPI_CALL enumerate_version(uint32_t *version)
{
    *version = loader_version;
    return VK_SUCCESS;
}

int main(void)
{
    static const struct { uint32_t loader, request; } cases[] = {
        {VK_API_VERSION_1_0, VK_API_VERSION_1_1},
        {VK_MAKE_API_VERSION(0, 1, 1, 130), VK_API_VERSION_1_1},
        {VK_MAKE_API_VERSION(0, 1, 2, 198), VK_API_VERSION_1_2},
        {VK_API_VERSION_1_3, VK_API_VERSION_1_3},
        {VK_API_VERSION_1_4, VK_API_VERSION_1_4},
        {VK_MAKE_API_VERSION(0, 1, 5, 0), VK_API_VERSION_1_4},
    };
    vkEnumerateInstanceVersion = enumerate_version;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        loader_version = cases[i].loader;
        uint32_t request = hle_vk_instance_version();
        if (request != cases[i].request) {
            fprintf(stderr, "loader %#x: requested %#x, expected %#x\n",
                    loader_version, request, cases[i].request);
            return 1;
        }
    }
    loader_version = VK_API_VERSION_1_0;
    if (hle_vk_init() != -2 || strcmp(g_hvk.caps.missing, "Vulkan 1.1 loader") != 0) {
        fprintf(stderr, "expected a Vulkan 1.0 loader to be rejected before instance creation\n");
        return 1;
    }
    return 0;
}
