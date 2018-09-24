/* Copyright (c) 2015-2018 The Khronos Group Inc.
 * Copyright (c) 2015-2018 Valve Corporation
 * Copyright (c) 2015-2018 LunarG, Inc.
 * Copyright (C) 2015-2018 Google Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Author: Cody Northrop <cnorthrop@google.com>
 * Author: Michael Lentine <mlentine@google.com>
 * Author: Tobin Ehlis <tobine@google.com>
 * Author: Chia-I Wu <olv@google.com>
 * Author: Chris Forbes <chrisf@ijw.co.nz>
 * Author: Mark Lobodzinski <mark@lunarg.com>
 * Author: Ian Elliott <ianelliott@google.com>
 * Author: Dave Houlton <daveh@lunarg.com>
 * Author: Dustin Graves <dustin@lunarg.com>
 * Author: Jeremy Hayes <jeremy@lunarg.com>
 * Author: Jon Ashburn <jon@lunarg.com>
 * Author: Karl Schultz <karl@lunarg.com>
 * Author: Mark Young <marky@lunarg.com>
 * Author: Mike Schuchardt <mikes@lunarg.com>
 * Author: Mike Weiblen <mikew@lunarg.com>
 * Author: Tony Barbour <tony@LunarG.com>
 */


 // Allow use of STL min and max functions in Windows
#define NOMINMAX
#define VALIDATION_ERROR_MAP_IMPL

////#define UBER_LAYER 1

#include <algorithm>
#include <array>
#include <assert.h>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <valarray>

#include "vk_loader_platform.h"
#include "vk_dispatch_table_helper.h"
#include "vk_enum_string_helper.h"
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wwrite-strings"
#endif
#if defined(__GNUC__)
#pragma GCC diagnostic warning "-Wwrite-strings"
#endif

#include "object_lifetimes.h"

#include "core_validation.h"
#include "buffer_validation.h"
#include "shader_validation.h"
#include "vk_layer_data.h"
#include "vk_layer_extension_utils.h"
#include "vk_layer_utils.h"
#include "vk_typemap_helper.h"

namespace object_tracker {
#include "precall.h"
#include "postcall.h"
}


namespace core_validation {

using std::max;
using std::string;
using std::stringstream;
using std::unique_ptr;
using std::unordered_map;
using std::unordered_set;
using std::vector;

struct instance_layer_data {
    VkInstance instance = VK_NULL_HANDLE;
    debug_report_data *report_data = nullptr;
    vector<VkDebugReportCallbackEXT> logging_callback;
    vector<VkDebugUtilsMessengerEXT> logging_messenger;
    // The following are for keeping track of the temporary callbacks that can be used in vkCreateInstance and vkDestroyInstance
    uint32_t num_tmp_report_callbacks;
    VkDebugReportCallbackCreateInfoEXT *tmp_report_create_infos;
    VkDebugReportCallbackEXT *tmp_report_callbacks;
    uint32_t num_tmp_debug_messengers;
    VkDebugUtilsMessengerCreateInfoEXT *tmp_messenger_create_infos;
    VkDebugUtilsMessengerEXT *tmp_debug_messengers;

    VkLayerInstanceDispatchTable dispatch_table;

    CALL_STATE vkEnumeratePhysicalDevicesState = UNCALLED;
    uint32_t physical_devices_count = 0;
    CALL_STATE vkEnumeratePhysicalDeviceGroupsState = UNCALLED;
    uint32_t physical_device_groups_count = 0;
    CHECK_DISABLED disabled = {};

    unordered_map<VkPhysicalDevice, PHYSICAL_DEVICE_STATE> physical_device_map;
    unordered_map<VkSurfaceKHR, SURFACE_STATE> surface_map;

    InstanceExtensions extensions;
    uint32_t api_version;
    object_tracker::object_lifetime objdata;
};

struct layer_data {
    debug_report_data *report_data = nullptr;
    VkLayerDispatchTable dispatch_table;

    DeviceExtensions extensions = {};
    unordered_set<VkQueue> queues;  // All queues under given device
    // Layer specific data
    unordered_map<VkSampler, unique_ptr<SAMPLER_STATE>> samplerMap;
    unordered_map<VkImageView, unique_ptr<IMAGE_VIEW_STATE>> imageViewMap;
    unordered_map<VkImage, unique_ptr<IMAGE_STATE>> imageMap;
    unordered_map<VkBufferView, unique_ptr<BUFFER_VIEW_STATE>> bufferViewMap;
    unordered_map<VkBuffer, unique_ptr<BUFFER_STATE>> bufferMap;
    unordered_map<VkPipeline, unique_ptr<PIPELINE_STATE>> pipelineMap;
    unordered_map<VkCommandPool, COMMAND_POOL_NODE> commandPoolMap;
    unordered_map<VkDescriptorPool, DESCRIPTOR_POOL_STATE *> descriptorPoolMap;
    unordered_map<VkDescriptorSet, cvdescriptorset::DescriptorSet *> setMap;
    unordered_map<VkDescriptorSetLayout, std::shared_ptr<cvdescriptorset::DescriptorSetLayout>> descriptorSetLayoutMap;
    unordered_map<VkPipelineLayout, PIPELINE_LAYOUT_NODE> pipelineLayoutMap;
    unordered_map<VkDeviceMemory, unique_ptr<DEVICE_MEM_INFO>> memObjMap;
    unordered_map<VkFence, FENCE_NODE> fenceMap;
    unordered_map<VkQueue, QUEUE_STATE> queueMap;
    unordered_map<VkEvent, EVENT_STATE> eventMap;
    unordered_map<QueryObject, bool> queryToStateMap;
    unordered_map<VkQueryPool, QUERY_POOL_NODE> queryPoolMap;
    unordered_map<VkSemaphore, SEMAPHORE_NODE> semaphoreMap;
    unordered_map<VkCommandBuffer, GLOBAL_CB_NODE *> commandBufferMap;
    unordered_map<VkFramebuffer, unique_ptr<FRAMEBUFFER_STATE>> frameBufferMap;
    unordered_map<VkImage, vector<ImageSubresourcePair>> imageSubresourceMap;
    unordered_map<ImageSubresourcePair, IMAGE_LAYOUT_NODE> imageLayoutMap;
    unordered_map<VkRenderPass, std::shared_ptr<RENDER_PASS_STATE>> renderPassMap;
    unordered_map<VkShaderModule, unique_ptr<shader_module>> shaderModuleMap;
    unordered_map<VkDescriptorUpdateTemplateKHR, unique_ptr<TEMPLATE_STATE>> desc_template_map;
    unordered_map<VkSwapchainKHR, std::unique_ptr<SWAPCHAIN_NODE>> swapchainMap;
    GlobalQFOTransferBarrierMap<VkImageMemoryBarrier> qfo_release_image_barrier_map;
    GlobalQFOTransferBarrierMap<VkBufferMemoryBarrier> qfo_release_buffer_barrier_map;

    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;

    instance_layer_data *instance_data = nullptr;  // from device to enclosing instance

    DeviceFeatures enabled_features = {};
    // Device specific data
    PHYS_DEV_PROPERTIES_NODE phys_dev_properties = {};
    VkPhysicalDeviceMemoryProperties phys_dev_mem_props = {};
    VkPhysicalDeviceProperties phys_dev_props = {};
    // Device extension properties -- storing properties gathered from VkPhysicalDeviceProperties2KHR::pNext chain
    struct DeviceExtensionProperties {
        uint32_t max_push_descriptors;  // from VkPhysicalDevicePushDescriptorPropertiesKHR::maxPushDescriptors
        VkPhysicalDeviceDescriptorIndexingPropertiesEXT descriptor_indexing_props;
    };
    DeviceExtensionProperties phys_dev_ext_props = {};
    bool external_sync_warning = false;
    uint32_t api_version = 0;
    object_tracker::object_lifetime objdata;
};

}  // namespace core_validation

extern std::unordered_map<void *, core_validation::layer_data *> layer_data_map;
extern std::unordered_map<void *, core_validation::instance_layer_data *> instance_layer_data_map;

