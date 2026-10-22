/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "gpu_menu.hpp"
#if defined(SLOPFIN_GPU_MENU)
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "menu_shader.hpp"
#include "trace.hpp"
#include <algorithm>
#include <chrono>
#include <array>
#include <atomic>
#include <cstring>
#include <map>
#include <vector>
#include <string>
#include <nmmintrin.h>
#include <unistd.h>
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#if defined(SLOPFIN_HOST)
#include <dlfcn.h>
#else
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance,
                                                                              const char *);
#endif
namespace slopfin::gpu_menu
{
namespace
{
// clang-format off
#define GPU_FUNCTIONS(X) \
    X(EnumerateDeviceExtensionProperties) \
    X(GetMemoryHostPointerPropertiesEXT) \
    X(DestroyInstance) \
    X(FreeMemory) \
    X(GetDeviceQueue) \
    X(CreateShaderModule) \
    X(QueueWaitIdle) \
    X(EnumeratePhysicalDevices) \
    X(GetPhysicalDeviceQueueFamilyProperties) \
    X(GetPhysicalDeviceMemoryProperties) \
    X(CreateDevice) \
    X(GetDeviceProcAddr) \
    X(DestroyDevice) \
    X(CreateCommandPool) \
    X(DestroyCommandPool) \
    X(AllocateCommandBuffers) \
    X(ResetCommandBuffer) \
    X(BeginCommandBuffer) \
    X(EndCommandBuffer) \
    X(QueueSubmit) \
    X(CreateFence) \
    X(DestroyFence) \
    X(ResetFences) \
    X(WaitForFences) \
    X(CreateBuffer) \
    X(DestroyBuffer) \
    X(GetBufferMemoryRequirements) \
    X(AllocateMemory) \
    X(BindBufferMemory) \
    X(MapMemory) \
    X(UnmapMemory) \
    X(FlushMappedMemoryRanges) \
    X(InvalidateMappedMemoryRanges) \
    X(DestroyShaderModule) \
    X(CreateDescriptorSetLayout) \
    X(DestroyDescriptorSetLayout) \
    X(CreateDescriptorPool) \
    X(DestroyDescriptorPool) \
    X(AllocateDescriptorSets) \
    X(FreeDescriptorSets) \
    X(UpdateDescriptorSets) \
    X(CreatePipelineLayout) \
    X(DestroyPipelineLayout) \
    X(CreateComputePipelines) \
    X(DestroyPipeline) \
    X(CmdBindPipeline) \
    X(CmdBindDescriptorSets) \
    X(CmdPushConstants) \
    X(CmdDispatch) \
    X(CmdPipelineBarrier) \
    X(CreateQueryPool) \
    X(DestroyQueryPool) \
    X(CmdResetQueryPool) \
    X(CmdWriteTimestamp) \
    X(GetQueryPoolResults) \
    X(GetPhysicalDeviceProperties)
// clang-format on
#define DECLARE(n) PFN_vk##n vk##n = nullptr;
GPU_FUNCTIONS(DECLARE)
#undef DECLARE
struct Buffer
{
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    void *map = nullptr;
    std::size_t bytes = 0;
};
struct Texture
{
    Buffer data;
    VkDescriptorSet set{};
    std::uint64_t used = 0;
};
VkInstance instance{};
VkPhysicalDevice physical{};
VkDevice device{};
VkQueue queue{};
VkPhysicalDeviceMemoryProperties memory{};
VkCommandPool pool{};
VkCommandBuffer cmd{}; /* the slot being recorded */
/*
 * Submissions in flight. This console starts each one about a refresh after
 * it is made, though the work itself takes under 2 ms at 4K (PS5_Vulkan R68;
 * measured here with timestamps), so the CPU never waits for a frame it has
 * just submitted: it records the next into another slot while gfx's flip
 * thread waits for this one and flips it.
 */
constexpr unsigned kSlots = 3;
struct Slot
{
    VkCommandBuffer cmd{};
    VkFence fence{};
    bool submitted = false;
    std::uint64_t submitted_at = 0;
    std::atomic<bool> held{false}; /* the flip thread has yet to see it finish */
};
std::array<Slot, kSlots> slots;
unsigned current = 0, last_slot = 0;
std::atomic<std::uint64_t> latency_us{0}, latency_samples{0};
/* Two GPU timestamps around each submission, per slot: how long the GPU
   itself spent, as against how long it took to start and finish. */
VkQueryPool queries{};
double timestamp_ns = 0;
VkDescriptorSetLayout set_layout{};
VkDescriptorPool descriptors{};
VkPipelineLayout layout{};
VkPipeline pipeline{};
Buffer output, dummy;
std::map<std::pair<std::uint64_t, std::size_t>, Texture> textures;
std::map<void *, Texture> scanout_buffers;
/*
 * Text and icons. Their coverage masks live together in one atlas, uploaded
 * once per glyph, and are drawn in batches: one dispatch for a run of glyphs
 * that overlap nothing written since the last barrier, instead of one each.
 */
struct GlyphInstance
{
    int clip[4];
    int rect[4];
    std::uint32_t data[4]; /* from, to, atlas byte offset, kind */
};
static_assert(sizeof(GlyphInstance) == 48);
constexpr std::size_t kAtlasBytes = 32u * 1024u * 1024u;
constexpr std::uint32_t kMaxGlyphs = 32768;
constexpr std::uint32_t kMaxBatch = 4096;
Buffer atlas, glyph_buffer;
VkDescriptorSet atlas_set{};
std::size_t atlas_used = 0;
std::map<std::uint64_t, std::uint32_t> atlas_offsets;
std::uint32_t glyph_count = 0, batch_first = 0;
int batch_w = 0, batch_h = 0;
bool host_import = false;
Stats g_stats;
/*
 * Clip rectangles written since the last barrier. Each dispatch only touches
 * pixels inside its own clip, so one that overlaps none of these may run
 * alongside them; only an overlap needs the previous writes to land first.
 * Glyphs along a line, cards in a row and most panels never overlap.
 */
std::vector<std::array<int, 4>> unbarriered;
constexpr std::size_t kMaxUnbarriered = 256;
bool overlaps_unbarriered(const int *clip)
{
    for (const auto &r : unbarriered)
        if (clip[0] < r[2] && r[0] < clip[2] && clip[1] < r[3] && r[1] < clip[3])
            return true;
    return false;
}
std::uint64_t now_us()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}
std::size_t texture_bytes = 0;
std::uint64_t generation = 0;
bool ready = false, enabled = false, recording = false, cpu_dirty = true, gpu_dirty = false;
std::uint32_t *cpu_pixels = nullptr;
int width = 0, height = 0;

void note(const char *where, VkResult result)
{
    trace::mark(std::string{"GPU menu: "} + where + " = " + std::to_string(result));
}
bool ok(VkResult result, const char *where)
{
    if (result == VK_SUCCESS)
        return true;
    note(where, result);
    return false;
}
void release(Buffer &b)
{
    if (b.map)
        vkUnmapMemory(device, b.memory);
    if (b.buffer)
        vkDestroyBuffer(device, b.buffer, nullptr);
    if (b.memory)
        vkFreeMemory(device, b.memory, nullptr);
    b = {};
}
/*
 * `fast`: memory the GPU reaches at full speed, for buffers it reads or writes
 * every frame and the CPU only ever writes. Otherwise CPU-cached memory, for
 * buffers the CPU reads back (checksummed textures are compared byte for
 * byte), which the GPU reaches through the CPU's caches, more slowly.
 */
bool allocate(Buffer &b, std::size_t bytes, bool fast = false)
{
    b.bytes = std::max<std::size_t>(4, (bytes + 3) & ~std::size_t{3});
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = b.bytes;
    ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (!ok(vkCreateBuffer(device, &ci, nullptr, &b.buffer), "buffer"))
        return false;
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device, b.buffer, &req);
    unsigned type = memory.memoryTypeCount;
    int best = -1;
    for (unsigned i = 0; i < memory.memoryTypeCount; i++)
    {
        const auto flags = memory.memoryTypes[i].propertyFlags;
        if (!(req.memoryTypeBits & (1u << i)) || !(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
            continue;
        const bool local = flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        const bool cached = flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        const int score = fast ? (local ? 2 : 0) + (cached ? 0 : 1) : (cached ? 1 : 0);
        if (score > best)
        {
            best = score;
            type = i;
        }
    }
    if (type == memory.memoryTypeCount)
        return false;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    return ok(vkAllocateMemory(device, &ai, nullptr, &b.memory), "memory") &&
           ok(vkBindBufferMemory(device, b.buffer, b.memory, 0), "bind") &&
           ok(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.map), "map");
}
void flush_memory(const Buffer &b, bool invalidate = false)
{
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = b.memory;
    range.size = VK_WHOLE_SIZE;
    if (invalidate)
        vkInvalidateMappedMemoryRanges(device, 1, &range);
    else
        vkFlushMappedMemoryRanges(device, 1, &range);
}
/* Every set: the surface written, the pixels read, and the glyph batch list. */
void write_set(VkDescriptorSet set, const Buffer &first, const Buffer &second)
{
    VkDescriptorBufferInfo infos[3] = {{first.buffer, 0, first.bytes},
                                       {second.buffer, 0, second.bytes},
                                       {glyph_buffer.buffer, 0, glyph_buffer.bytes}};
    VkWriteDescriptorSet writes[3]{};
    for (unsigned i = 0; i < 3; i++)
    {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = infos + i;
    }
    vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);
}
bool allocate_set(VkDescriptorSet &set)
{
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = descriptors;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &set_layout;
    return ok(vkAllocateDescriptorSets(device, &ai, &set), "descriptor");
}
void wait_for_writes()
{
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0,
                         nullptr);
    unbarriered.clear();
    ++g_stats.barriers;
}
/* Dispatches the glyphs queued since the last batch, all at once. */
void flush_batch()
{
    if (glyph_count == batch_first)
        return;
    Command p{};
    p.info[0] = glyph_batch;
    p.info[1] = width;
    p.info[3] = static_cast<int>(batch_first);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &atlas_set, 0,
                            nullptr);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    vkCmdDispatch(cmd, static_cast<unsigned>(batch_w + 7) / 8,
                  static_cast<unsigned>(batch_h + 7) / 8, glyph_count - batch_first);
    ++g_stats.dispatches;
    batch_first = glyph_count;
    batch_w = batch_h = 0;
}
__attribute__((target("sse4.2"))) std::uint64_t hash_bytes(const void *pixels, std::size_t bytes)
{
    auto *p = static_cast<const unsigned char *>(pixels);
    std::uint64_t hash = bytes;
    for (; bytes >= 8; bytes -= 8, p += 8)
    {
        std::uint64_t v;
        std::memcpy(&v, p, 8);
        hash = _mm_crc32_u64(hash, v);
    }
    for (; bytes; --bytes, ++p)
        hash = _mm_crc32_u8(static_cast<unsigned>(hash), *p);
    return hash;
}
/* Waits until a slot's last submission has finished and the flip thread
   is done with it, so it can be recorded again. */
bool retire(Slot &slot)
{
    while (slot.held.load(std::memory_order_acquire))
        usleep(100);
    if (!slot.submitted)
        return true;
    const std::uint64_t began = now_us();
    if (!ok(vkWaitForFences(device, 1, &slot.fence, VK_TRUE, 2000000000ull), "wait"))
        return false;
    g_stats.gpu_wait_us += now_us() - began;
    slot.submitted = false;
    if (queries)
    {
        const auto first = static_cast<unsigned>(&slot - slots.data()) * 2;
        std::uint64_t stamps[2]{};
        if (vkGetQueryPoolResults(device, queries, first, 2, sizeof(stamps), stamps,
                                  sizeof(stamps[0]), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
            stamps[1] > stamps[0])
            g_stats.gpu_busy_us += static_cast<std::uint64_t>(
                static_cast<double>(stamps[1] - stamps[0]) * timestamp_ns / 1000.0);
    }
    return true;
}
bool wait_all()
{
    for (auto &slot : slots)
        if (!retire(slot))
            return false;
    return true;
}
/* Submits what was recorded. `wait`: and everything before it has finished,
   for the CPU to read or rewrite what the GPU uses. */
bool submit(bool wait = true)
{
    if (!recording)
        return !wait || wait_all();
    flush_batch();
    if (glyph_count != current * kMaxGlyphs)
        flush_memory(glyph_buffer);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                         1, &barrier, 0, nullptr, 0, nullptr);
    if (queries)
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, current * 2 + 1);
    recording = false;
    if (!ok(vkEndCommandBuffer(cmd), "end"))
        return false;
    Slot &slot = slots[current];
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &slot.cmd;
    if (!ok(vkResetFences(device, 1, &slot.fence), "reset fence") ||
        !ok(vkQueueSubmit(queue, 1, &si, slot.fence), "submit"))
        return false;
    slot.submitted = true;
    slot.submitted_at = now_us();
    last_slot = current;
    current = (current + 1) % kSlots;
    ++g_stats.submits;
    ++generation;
    // Evict only what no submission still in flight can use (those are the
    // last kSlots); cap persistent artwork.
    for (auto it = textures.begin(); texture_bytes > 192u * 1024u * 1024u && it != textures.end();)
    {
        if (it->second.used + kSlots < generation)
        {
            texture_bytes -= it->second.data.bytes;
            vkFreeDescriptorSets(device, descriptors, 1, &it->second.set);
            release(it->second.data);
            it = textures.erase(it);
        }
        else
            ++it;
    }
    return !wait || wait_all();
}
bool begin()
{
    if (recording)
        return true;
    Slot &slot = slots[current];
    if (!retire(slot))
        return false;
    if (cpu_dirty && cpu_pixels)
    {
        if (!wait_all())
            return false;
        g_stats.uploaded_bytes += output.bytes;
        std::memcpy(output.map, cpu_pixels, output.bytes);
        flush_memory(output);
        cpu_dirty = false;
    }
    cmd = slot.cmd;
    if (!ok(vkResetCommandBuffer(cmd, 0), "reset"))
        return false;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!ok(vkBeginCommandBuffer(cmd, &bi), "begin"))
        return false;
    if (queries)
    {
        vkCmdResetQueryPool(cmd, queries, current * 2, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, current * 2);
    }
    /* After the CPU's writes, and after every earlier submission's: they are
       no longer waited for, so the last one's scanout may still be reading
       the surface this one starts by overwriting. */
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0,
                         nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    recording = true;
    unbarriered.clear();
    glyph_count = batch_first = current * kMaxGlyphs;
    batch_w = batch_h = 0;

    return true;
}
Texture *upload(const void *pixels, std::size_t bytes, std::pair<std::uint64_t, std::size_t> key,
                bool fast)
{
    // Bound both descriptors and memory, including bursts before the next fence.
    if (textures.size() >= 4095 || texture_bytes + bytes > 256u * 1024u * 1024u)
        return nullptr;
    Texture t{};
    t.used = generation;
    if (!allocate(t.data, bytes, fast))
    {
        release(t.data);
        return nullptr;
    }
    auto *bytes_out = static_cast<unsigned char *>(t.data.map);
    if (pixels)
    {
        std::memcpy(bytes_out, pixels, bytes);
        g_stats.uploaded_bytes += bytes;
    }
    else
        std::memset(bytes_out, 0, bytes);
    std::memset(bytes_out + bytes, 0, t.data.bytes - bytes); /* the word padding */
    flush_memory(t.data);
    if (!allocate_set(t.set))
    {
        release(t.data);
        return nullptr;
    }
    write_set(t.set, output, t.data);
    texture_bytes += t.data.bytes;
    return &textures.emplace(key, t).first->second;
}
Texture *texture(const void *pixels, std::size_t bytes, std::uint64_t id = 0)
{
    /* Named pixels are looked up by name: the top bit of the size keeps their
       keys apart from checksummed ones, whose sizes never reach it. */
    constexpr std::size_t kNamed = std::size_t{1} << 63;
    if (id != 0 && pixels)
    {
        const auto key = std::make_pair(id, bytes | kNamed);
        if (auto found = textures.find(key); found != textures.end())
        {
            found->second.used = generation;
            return &found->second;
        }
        return upload(pixels, bytes, key, true);
    }
    if (pixels)
        g_stats.hashed_bytes += bytes;
    auto key = std::make_pair(pixels ? hash_bytes(pixels, bytes) : 0, bytes);
    auto found = textures.find(key);
    while (found != textures.end())
    {
        // A checksum chooses a bucket; equality still compares every byte.
        if (pixels)
            g_stats.hashed_bytes += bytes;
        if (!pixels || std::memcmp(found->second.data.map, pixels, bytes) == 0)
        {
            found->second.used = generation;
            return &found->second;
        }
        ++key.first;
        found = textures.find(key);
    }
    return upload(pixels, bytes, key, false);
}
/*
 * Where a named mask sits in the atlas, uploading it the first time. When the
 * atlas fills, the work recorded so far (which reads it) is run to completion
 * and the atlas starts over.
 */
bool atlas_offset(const void *pixels, std::size_t bytes, std::uint64_t id, std::uint32_t &offset)
{
    if (bytes == 0 || bytes > kAtlasBytes)
        return false;
    if (auto found = atlas_offsets.find(id); found != atlas_offsets.end())
    {
        offset = found->second;
        return true;
    }
    const std::size_t padded = (bytes + 3) & ~std::size_t{3};
    if (atlas_used + padded > kAtlasBytes)
    {
        if (!submit())
            return false;
        atlas_offsets.clear();
        atlas_used = 0;
        trace::mark("GPU menu: glyph atlas full; started over");
    }
    offset = static_cast<std::uint32_t>(atlas_used);
    auto *at = static_cast<unsigned char *>(atlas.map) + atlas_used;
    std::memcpy(at, pixels, bytes);
    std::memset(at + bytes, 0, padded - bytes);
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = atlas.memory;
    range.offset = atlas_used & ~std::size_t{255};
    range.size = VK_WHOLE_SIZE;
    vkFlushMappedMemoryRanges(device, 1, &range);
    atlas_used += padded;
    g_stats.uploaded_bytes += bytes;
    atlas_offsets.emplace(id, offset);
    return true;
}
/* Queues one named coverage mask into the current batch. */
bool queue_glyph(const Command &input, const void *pixels, std::size_t bytes, std::uint64_t id)
{
    std::uint32_t offset = 0;
    if (!atlas_offset(pixels, bytes, id, offset) || !begin())
        return false;
    if (glyph_count >= (current + 1) * kMaxGlyphs)
        return false; /* the caller draws it on its own */
    if (overlaps_unbarriered(input.clip) || unbarriered.size() >= kMaxUnbarriered)
    {
        flush_batch();
        wait_for_writes();
    }
    unbarriered.push_back({input.clip[0], input.clip[1], input.clip[2], input.clip[3]});
    GlyphInstance g{};
    std::memcpy(g.clip, input.clip, sizeof(g.clip));
    std::memcpy(g.rect, input.rect, sizeof(g.rect));
    g.data[0] = input.colours[0];
    g.data[1] = input.colours[1];
    g.data[2] = offset;
    g.data[3] = static_cast<std::uint32_t>(input.info[0]);
    std::memcpy(static_cast<GlyphInstance *>(glyph_buffer.map) + glyph_count, &g, sizeof(g));
    ++glyph_count;
    ++g_stats.glyphs;
    batch_w = std::max(batch_w, input.clip[2] - input.clip[0]);
    batch_h = std::max(batch_h, input.clip[3] - input.clip[1]);
    if (glyph_count - batch_first >= kMaxBatch)
        flush_batch();
    gpu_dirty = true;
    return true;
}
} // namespace
bool initialize(int w, int h) noexcept
{
    if (ready)
        return true;
    width = w;
    height = h;
#if defined(SLOPFIN_HOST)
    void *library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    const auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        library ? dlsym(library, "vkGetInstanceProcAddr") : nullptr);
#else
    const auto get = vk_icdGetInstanceProcAddr;
#endif
    if (!get)
        return false;
    auto create = reinterpret_cast<PFN_vkCreateInstance>(get(VK_NULL_HANDLE, "vkCreateInstance"));
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "SlopFin 4K menu";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    if (!create || !ok(create(&ci, nullptr, &instance), "instance"))
        return false;
#define LOAD_INSTANCE(n) vk##n = reinterpret_cast<PFN_vk##n>(get(instance, "vk" #n));
    GPU_FUNCTIONS(LOAD_INSTANCE)
#undef LOAD_INSTANCE
    unsigned count = 0;
    if (!ok(vkEnumeratePhysicalDevices(instance, &count, nullptr), "physical count") || count == 0)
        return false;
    std::array<VkPhysicalDevice, 16> devices{};
    count = std::min(count, 16u);
    const auto physical_result = vkEnumeratePhysicalDevices(instance, &count, devices.data());
    if (physical_result != VK_SUCCESS && physical_result != VK_INCOMPLETE)
        return false;
    physical = devices[0];
    unsigned families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, nullptr);
    std::array<VkQueueFamilyProperties, 32> props{};
    families = std::min(families, 32u);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, props.data());
    unsigned family = 0;
    while (family < families && !(props[family].queueFlags & VK_QUEUE_COMPUTE_BIT))
        ++family;
    if (family == families)
        return false;
    float priority = 1;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    unsigned extension_count = 0;
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &extension_count, nullptr);
    std::array<VkExtensionProperties, 256> extensions{};
    extension_count = std::min(extension_count, 256u);
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &extension_count, extensions.data());
    for (unsigned i = 0; i < extension_count; i++)
        if (std::strcmp(extensions[i].extensionName, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME) ==
            0)
            host_import = true;
    const char *import_extension = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    if (host_import)
    {
        di.enabledExtensionCount = 1;
        di.ppEnabledExtensionNames = &import_extension;
    }
    if (!ok(vkCreateDevice(physical, &di, nullptr, &device), "device"))
        return false;
#define LOAD_DEVICE(n)                                                                             \
    if (auto f = vkGetDeviceProcAddr(device, "vk" #n))                                             \
        vk##n = reinterpret_cast<PFN_vk##n>(f);
    GPU_FUNCTIONS(LOAD_DEVICE)
#undef LOAD_DEVICE
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    for (unsigned i = 0; i < memory.memoryTypeCount; i++)
    {
        const auto flags = memory.memoryTypes[i].propertyFlags;
        const auto heap = memory.memoryTypes[i].heapIndex;
        trace::mark("GPU menu: memory type " + std::to_string(i) + " heap " + std::to_string(heap) +
                    " (" + std::to_string(memory.memoryHeaps[heap].size >> 20) + " MB)" +
                    ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? " local" : "") +
                    ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? " visible" : "") +
                    ((flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? " coherent" : "") +
                    ((flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? " cached" : ""));
    }
    vkGetDeviceQueue(device, family, 0, &queue);
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.queueFamilyIndex = family;
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (!ok(vkCreateCommandPool(device, &pi, nullptr, &pool), "command pool"))
        return false;
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ca.commandPool = pool;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1;
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    for (auto &slot : slots)
        if (!ok(vkAllocateCommandBuffers(device, &ca, &slot.cmd), "command") ||
            !ok(vkCreateFence(device, &fi, nullptr, &slot.fence), "fence"))
            return false;
    cmd = slots[0].cmd;
    {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical, &properties);
        timestamp_ns = properties.limits.timestampPeriod;
        VkQueryPoolCreateInfo qp{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qp.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qp.queryCount = 2 * kSlots;
        if (props[family].timestampValidBits == 0 || timestamp_ns <= 0 ||
            vkCreateQueryPool(device, &qp, nullptr, &queries) != VK_SUCCESS)
            queries = {};
        trace::mark("GPU menu: timestamps " + std::string{queries ? "on" : "off"} + ", " +
                    std::to_string(timestamp_ns) + " ns a tick");
    }
    if (!allocate(output, static_cast<std::size_t>(w) * h * 4, true) ||
        !allocate(atlas, kAtlasBytes, true) ||
        !allocate(glyph_buffer, kSlots * kMaxGlyphs * sizeof(GlyphInstance), true))
        return false;
    std::memset(output.map, 0, output.bytes);
    flush_memory(output);
    VkDescriptorSetLayoutBinding bindings[3]{};
    for (int i = 0; i < 3; i++)
        bindings[i] = {static_cast<unsigned>(i), VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                       VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 3;
    li.pBindings = bindings;
    if (!ok(vkCreateDescriptorSetLayout(device, &li, nullptr, &set_layout), "set layout"))
        return false;
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * 4160};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dp.maxSets = 4160;
    dp.poolSizeCount = 1;
    dp.pPoolSizes = &size;
    if (!ok(vkCreateDescriptorPool(device, &dp, nullptr, &descriptors), "descriptors") ||
        !allocate_set(atlas_set))
        return false;
    write_set(atlas_set, output, atlas);
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Command)};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &set_layout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &range;
    if (!ok(vkCreatePipelineLayout(device, &pl, nullptr, &layout), "pipeline layout"))
        return false;
    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sm.codeSize = sizeof(slopfin_menu_shader);
    sm.pCode = slopfin_menu_shader;
    VkShaderModule shader{};
    if (!ok(vkCreateShaderModule(device, &sm, nullptr, &shader), "shader"))
        return false;
    VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cp.layout = layout;
    cp.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                nullptr,
                0,
                VK_SHADER_STAGE_COMPUTE_BIT,
                shader,
                "main",
                nullptr};
    const auto result =
        vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cp, nullptr, &pipeline);
    vkDestroyShaderModule(device, shader, nullptr);
    if (!ok(result, "pipeline"))
        return false;
    ready = enabled = true;
    trace::mark("GPU menu: compute compositor ready at " + std::to_string(w) + "x" +
                std::to_string(h));
    return true;
}
void shutdown() noexcept
{
    if (device)
    {
        if (recording)
            submit();
        if (queue)
            vkQueueWaitIdle(queue);
        for (auto &[key, t] : scanout_buffers)
            release(t.data);
        scanout_buffers.clear();
        for (auto &[key, t] : textures)
            release(t.data);
        textures.clear();
        texture_bytes = 0;
        release(output);
        release(dummy);
        release(atlas);
        release(glyph_buffer);
        atlas_offsets.clear();
        atlas_used = 0;
        if (pipeline)
            vkDestroyPipeline(device, pipeline, nullptr);
        if (layout)
            vkDestroyPipelineLayout(device, layout, nullptr);
        if (descriptors)
            vkDestroyDescriptorPool(device, descriptors, nullptr);
        if (set_layout)
            vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
        for (auto &slot : slots)
        {
            if (slot.fence)
                vkDestroyFence(device, slot.fence, nullptr);
            slot.fence = {};
            slot.cmd = {};
            slot.submitted = false;
            slot.held = false;
        }
        current = last_slot = 0;
        if (queries)
            vkDestroyQueryPool(device, queries, nullptr);
        queries = {};
        if (pool)
            vkDestroyCommandPool(device, pool, nullptr);
        vkDestroyDevice(device, nullptr);
    }
    if (instance)
        vkDestroyInstance(instance, nullptr);
    device = {};
    instance = {};
    ready = enabled = recording = false;
}
void enable(bool value) noexcept
{
    if (value && !enabled)
        cpu_dirty = true;
    enabled = ready && value;
}
bool active() noexcept
{
    return ready && enabled;
}
bool draw(const Command &input, const void *pixels, std::size_t bytes, std::uint64_t id) noexcept
{
    if (!active())
        return false;
    if (input.clip[0] >= input.clip[2] || input.clip[1] >= input.clip[3])
        return true;
    if (id != 0 && pixels && (input.info[0] == mask || input.info[0] == mask_gradient) &&
        queue_glyph(input, pixels, bytes, id))
        return true;
    Texture *t = texture(pixels, bytes, id);
    if (!t)
        return false;
    if (!begin())
    {
        enabled = false;
        return false;
    }
    // Queued glyphs come first; then an earlier write this blend reads must
    // land first, while disjoint ones need not.
    flush_batch();
    if (overlaps_unbarriered(input.clip) || unbarriered.size() >= kMaxUnbarriered)
        wait_for_writes();
    unbarriered.push_back({input.clip[0], input.clip[1], input.clip[2], input.clip[3]});
    Command p = input;
    p.info[1] = width;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &t->set, 0, nullptr);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    vkCmdDispatch(cmd, (p.clip[2] - p.clip[0] + 7) / 8, (p.clip[3] - p.clip[1] + 7) / 8, 1);
    gpu_dirty = true;
    ++g_stats.dispatches;

    return true;
}
int present(std::uint32_t *frame, std::size_t bytes, bool wait) noexcept
{
    if (!active() || !host_import || !vkGetMemoryHostPointerPropertiesEXT)
        return -1;
    auto found = scanout_buffers.find(frame);
    if (found == scanout_buffers.end())
    {
        Texture t{};
        t.data.bytes = bytes;
        VkExternalMemoryBufferCreateInfo external{
            VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        ci.pNext = &external;
        ci.size = bytes;
        ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        if (!ok(vkCreateBuffer(device, &ci, nullptr, &t.data.buffer), "scanout buffer"))
            return -1;
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device, t.data.buffer, &req);
        VkMemoryHostPointerPropertiesEXT props{
            VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
        if (req.size > bytes ||
            !ok(vkGetMemoryHostPointerPropertiesEXT(
                    device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, frame, &props),
                "scanout pointer"))
        {
            release(t.data);
            return -1;
        }
        unsigned type = 0;
        while (type < memory.memoryTypeCount &&
               !(req.memoryTypeBits & props.memoryTypeBits & (1u << type)))
            ++type;
        if (type == memory.memoryTypeCount)
        {
            release(t.data);
            return -1;
        }
        VkImportMemoryHostPointerInfoEXT import{
            VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
        import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        import.pHostPointer = frame;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.pNext = &import;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        if (!ok(vkAllocateMemory(device, &ai, nullptr, &t.data.memory), "scanout import") ||
            !ok(vkBindBufferMemory(device, t.data.buffer, t.data.memory, 0), "scanout bind"))
        {
            release(t.data);
            return -1;
        }
        if (!allocate_set(t.set))
        {
            release(t.data);
            return -1;
        }
        write_set(t.set, t.data, output);
        found = scanout_buffers.emplace(frame, t).first;
        trace::mark("GPU menu: direct tiled scanout imported");
    }
    if (!begin())
        return -1;
    flush_batch();
    wait_for_writes();
    Command p{};
    p.clip[2] = width;
    p.clip[3] = height;
    p.info[0] = scanout;
    p.info[1] = width;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &found->second.set,
                            0, nullptr);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    vkCmdDispatch(cmd, (width + 7) / 8, (height + 7) / 8, 1);
    if (!submit(wait))
        return -1;
    if (!wait)
        slots[last_slot].held.store(true, std::memory_order_release);
    return static_cast<int>(last_slot);
}
void wait_submission(int index) noexcept
{
    if (index < 0 || index >= static_cast<int>(kSlots))
        return;
    Slot &slot = slots[static_cast<unsigned>(index)];
    if (vkWaitForFences(device, 1, &slot.fence, VK_TRUE, 2000000000ull) == VK_SUCCESS)
    {
        latency_us += now_us() - slot.submitted_at;
        ++latency_samples;
    }
    slot.held.store(false, std::memory_order_release);
}
void finish(std::uint32_t *pixels) noexcept
{
    cpu_pixels = pixels;
    if (!submit())
    {
        enabled = ready = false;
        trace::mark("GPU menu: submission failed; CPU fallback");
    }
    if (gpu_dirty)
    {
        flush_memory(output, true);
        g_stats.readback_bytes += output.bytes;
        std::memcpy(pixels, output.map, output.bytes);
        gpu_dirty = false;
    }
}
void cpu_access(std::uint32_t *pixels) noexcept
{
    if (ready)
    {
        ++g_stats.cpu_syncs;
        finish(pixels);
        cpu_dirty = true;
    }
}
Stats take_stats() noexcept
{
    Stats taken = g_stats;
    g_stats = {};
    taken.gpu_latency_us = latency_us.exchange(0);
    taken.latency_samples = latency_samples.exchange(0);
    return taken;
}
} // namespace slopfin::gpu_menu
#endif
