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
 * Author: Mark Lobodzinski <mark@lunarg.com>
 * Author: Jon Ashburn <jon@lunarg.com>
 * Author: Tobin Ehlis <tobin@lunarg.com>
 */

#include <mutex>
#include <cinttypes>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unordered_map>
#include <unordered_set>

#include "vulkan/vulkan.h"
#include "vk_object_types.h"
#include "vk_layer_logging.h"
//#include "object_lifetimes.h"

namespace object_tracker {

#include "precall.h"
#include "postcall.h"

// Suppress unused warning on Linux
#if defined(__GNUC__)
#define DECORATE_UNUSED __attribute__((unused))
#else
#define DECORATE_UNUSED
#endif

// clang-format off
static const char DECORATE_UNUSED *kVUID_ObjectTracker_Info = "UNASSIGNED-ObjectTracker-Info";
static const char DECORATE_UNUSED *kVUID_ObjectTracker_InternalError = "UNASSIGNED-ObjectTracker-InternalError";
static const char DECORATE_UNUSED *kVUID_ObjectTracker_ObjectLeak =    "UNASSIGNED-ObjectTracker-ObjectLeak";
static const char DECORATE_UNUSED *kVUID_ObjectTracker_UnknownObject = "UNASSIGNED-ObjectTracker-UnknownObject";
// clang-format on

#undef DECORATE_UNUSED

extern uint64_t object_track_index;

bool DeviceReportUndestroyedObjects(VkDevice device, VulkanObjectType object_type, const std::string &error_code);
void DeviceDestroyUndestroyedObjects(VkDevice device, VulkanObjectType object_type);
void CreateQueue(VkDevice device, VkQueue vkObj);
void AddQueueInfo(VkDevice device, uint32_t queue_node_index, VkQueue queue);
void ValidateQueueFlags(VkQueue queue, const char *function);
void AllocateCommandBuffer(VkDevice device, const VkCommandPool command_pool, const VkCommandBuffer command_buffer,
                           VkCommandBufferLevel level);
void AllocateDescriptorSet(VkDevice device, VkDescriptorPool descriptor_pool, VkDescriptorSet descriptor_set);
void CreateSwapchainImageObject(VkDevice dispatchable_object, VkImage swapchain_image, VkSwapchainKHR swapchain);
bool ReportUndestroyedObjects(VkDevice device, const std::string &error_code);
void DestroyUndestroyedObjects(VkDevice device);
bool ValidateDeviceObject(uint64_t device_handle, const std::string &invalid_handle_code, const std::string &wrong_device_code);

template <typename T1, typename T2>
bool ValidateObject(T1 dispatchable_object, T2 object, VulkanObjectType object_type, bool null_allowed,
                    const std::string &invalid_handle_code, const std::string &wrong_device_code) {
    if (null_allowed && (object == VK_NULL_HANDLE)) {
        return false;
    }
    auto object_handle = HandleToUint64(object);

    if (object_type == kVulkanObjectTypeDevice) {
        return ValidateDeviceObject(object_handle, invalid_handle_code, wrong_device_code);
    }

    VkDebugReportObjectTypeEXT debug_object_type = get_debug_report_enum[object_type];

    layer_data *device_data = GetLayerDataPtr(get_dispatch_key(dispatchable_object), layer_data_map);
    // Look for object in device object map
    if (device_data->objdata.object_map[object_type].find(object_handle) == device_data->objdata.object_map[object_type].end()) {
        // If object is an image, also look for it in the swapchain image map
        if ((object_type != kVulkanObjectTypeImage) ||
            (device_data->objdata.swapchainImageMap.find(object_handle) == device_data->objdata.swapchainImageMap.end())) {
            // Object not found, look for it in other device object maps
            for (auto other_device_data : layer_data_map) {
                if (other_device_data.second != device_data) {
                    if (other_device_data.second->objdata.object_map[object_type].find(object_handle) !=
                            other_device_data.second->objdata.object_map[object_type].end() ||
                        (object_type == kVulkanObjectTypeImage &&
                         other_device_data.second->objdata.swapchainImageMap.find(object_handle) !=
                             other_device_data.second->objdata.swapchainImageMap.end())) {
                        // Object found on other device, report an error if object has a device parent error code
                        if ((wrong_device_code != kVUIDUndefined) && (object_type != kVulkanObjectTypeSurfaceKHR)) {
                            return log_msg(device_data->report_data, VK_DEBUG_REPORT_ERROR_BIT_EXT, debug_object_type,
                                           object_handle, wrong_device_code,
                                           "Object 0x%" PRIxLEAST64
                                           " was not created, allocated or retrieved from the correct device.",
                                           object_handle);
                        } else {
                            return false;
                        }
                    }
                }
            }
            // Report an error if object was not found anywhere
            return log_msg(device_data->report_data, VK_DEBUG_REPORT_ERROR_BIT_EXT, debug_object_type, object_handle,
                           invalid_handle_code, "Invalid %s Object 0x%" PRIxLEAST64 ".", object_string[object_type], object_handle);
        }
    }
    return false;
}

// Accessor functions for debug report and object lifetime data -- hides dealing with separate instance/device data maps
template <typename T>
inline const debug_report_data *GetDebugReportData(T disp_obj) {
    return GetLayerDataPtr(get_dispatch_key(disp_obj), layer_data_map)->report_data;
}
template <>
inline const debug_report_data *GetDebugReportData<VkInstance>(VkInstance disp_obj) {
    return GetLayerDataPtr(get_dispatch_key(disp_obj), instance_layer_data_map)->report_data;
}
template <>
inline const debug_report_data *GetDebugReportData<VkPhysicalDevice>(VkPhysicalDevice disp_obj) {
    return GetLayerDataPtr(get_dispatch_key(disp_obj), instance_layer_data_map)->report_data;
}
template <typename T>
inline object_lifetime *GetObjLifetimeData(T disp_obj) {
    return &(GetLayerDataPtr(get_dispatch_key(disp_obj), layer_data_map)->objdata);
}
template <>
inline object_lifetime *GetObjLifetimeData<VkInstance>(VkInstance disp_obj) {
    return &(GetLayerDataPtr(get_dispatch_key(disp_obj), instance_layer_data_map)->objdata);
}
template <>
inline object_lifetime *GetObjLifetimeData<VkPhysicalDevice>(VkPhysicalDevice disp_obj) {
    return &(GetLayerDataPtr(get_dispatch_key(disp_obj), instance_layer_data_map)->objdata);
}

template <typename T1, typename T2>
void CreateObject(T1 dispatchable_object, T2 object, VulkanObjectType object_type, const VkAllocationCallbacks *pAllocator) {
    auto report_data = GetDebugReportData(dispatchable_object);
    auto obj_data = GetObjLifetimeData(dispatchable_object);

    auto object_handle = HandleToUint64(object);
    bool custom_allocator = pAllocator != nullptr;

    if (!obj_data->object_map[object_type].count(object_handle)) {
        VkDebugReportObjectTypeEXT debug_object_type = get_debug_report_enum[object_type];
        log_msg(report_data, VK_DEBUG_REPORT_INFORMATION_BIT_EXT, debug_object_type, object_handle, kVUID_ObjectTracker_Info,
                "OBJ[0x%" PRIxLEAST64 "] : CREATE %s object 0x%" PRIxLEAST64, object_track_index++, object_string[object_type],
                object_handle);

        ObjTrackState *pNewObjNode = new ObjTrackState;
        pNewObjNode->object_type = object_type;
        pNewObjNode->status = custom_allocator ? OBJSTATUS_CUSTOM_ALLOCATOR : OBJSTATUS_NONE;
        pNewObjNode->handle = object_handle;

        obj_data->object_map[object_type][object_handle] = pNewObjNode;
        obj_data->num_objects[object_type]++;
        obj_data->num_total_objects++;
    }
}

template <typename T1, typename T2>
void DestroyObjectSilently(T1 dispatchable_object, T2 object, VulkanObjectType object_type) {
    auto obj_data = GetObjLifetimeData(dispatchable_object);

    auto object_handle = HandleToUint64(object);
    assert(object_handle != VK_NULL_HANDLE);

    auto item = obj_data->object_map[object_type].find(object_handle);
    assert(item != obj_data->object_map[object_type].end());

    ObjTrackState *pNode = item->second;
    assert(obj_data->num_total_objects > 0);

    obj_data->num_total_objects--;
    assert(obj_data->num_objects[pNode->object_type] > 0);

    obj_data->num_objects[pNode->object_type]--;

    delete pNode;
    obj_data->object_map[object_type].erase(item);
}

template <typename T1, typename T2>
bool ValidateDestroyObject(T1 dispatchable_object, T2 object, VulkanObjectType object_type, const VkAllocationCallbacks *pAllocator,
                           const std::string &expected_custom_allocator_code, const std::string &expected_default_allocator_code) {
    auto report_data = GetDebugReportData(dispatchable_object);
    auto obj_data = GetObjLifetimeData(dispatchable_object);
    bool skip = false;

    auto object_handle = HandleToUint64(object);
    bool custom_allocator = pAllocator != nullptr;
    VkDebugReportObjectTypeEXT debug_object_type = get_debug_report_enum[object_type];

    if (object_handle != VK_NULL_HANDLE) {
        auto item = obj_data->object_map[object_type].find(object_handle);
        if (item != obj_data->object_map[object_type].end()) {
            ObjTrackState *pNode = item->second;

            skip |= log_msg(report_data, VK_DEBUG_REPORT_INFORMATION_BIT_EXT, debug_object_type, object_handle,
                            kVUID_ObjectTracker_Info,
                            "OBJ_STAT Destroy %s obj 0x%" PRIxLEAST64 " (%" PRIu64 " total objs remain & %" PRIu64 " %s objs).",
                            object_string[object_type], HandleToUint64(object), obj_data->num_total_objects - 1,
                            obj_data->num_objects[pNode->object_type] - 1, object_string[object_type]);

            auto allocated_with_custom = (pNode->status & OBJSTATUS_CUSTOM_ALLOCATOR) ? true : false;
            if (allocated_with_custom && !custom_allocator && expected_custom_allocator_code != kVUIDUndefined) {
                // This check only verifies that custom allocation callbacks were provided to both Create and Destroy calls,
                // it cannot verify that these allocation callbacks are compatible with each other.
                skip |= log_msg(
                    report_data, VK_DEBUG_REPORT_ERROR_BIT_EXT, debug_object_type, object_handle, expected_custom_allocator_code,
                    "Custom allocator not specified while destroying %s obj 0x%" PRIxLEAST64 " but specified at creation.",
                    object_string[object_type], object_handle);
            } else if (!allocated_with_custom && custom_allocator && expected_default_allocator_code != kVUIDUndefined) {
                skip |= log_msg(
                    report_data, VK_DEBUG_REPORT_ERROR_BIT_EXT, debug_object_type, object_handle, expected_default_allocator_code,
                    "Custom allocator specified while destroying %s obj 0x%" PRIxLEAST64 " but not specified at creation.",
                    object_string[object_type], object_handle);
            }
        }
    }
    return skip;
}

template <typename T1, typename T2>
void RecordDestroyObject(T1 dispatchable_object, T2 object, VulkanObjectType object_type) {
    auto obj_data = GetObjLifetimeData(dispatchable_object);

    auto object_handle = HandleToUint64(object);

    if (object_handle != VK_NULL_HANDLE) {
        auto item = obj_data->object_map[object_type].find(object_handle);
        if (item != obj_data->object_map[object_type].end()) {
            DestroyObjectSilently(dispatchable_object, object, object_type);
        }
    }
}

}  // namespace object_tracker
