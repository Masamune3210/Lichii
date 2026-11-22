// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <unordered_map>
#include <fmt/ranges.h>
#include <array>
#include <vector>
#include <fmt/format.h>
#include <boost/container/small_vector.hpp>

#include "common/assert.h"
#include "common/cityhash.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ampere/trap_guards.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/object_pool.h"
#include "video_core/control/channel_state.h"
#include "video_core/dirty_flags.h"
#include "video_core/dlss_contract.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/fire_emblem_upscale.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2_debug_control.h"
#include "video_core/nx2_fast_draw.h"
#include "video_core/nx2_profile.h"
#include "video_core/oot_network.h"
#include "video_core/shader_cache.h"
#include "video_core/shader_cache_v2/cache.h"
#include "video_core/shader_cache_v2/records.h"
#include "video_core/shader_environment.h"
#include "video_core/texture_cache/util.h"

namespace VideoCommon {

void ShaderCache::InvalidateRegion(VAddr addr, size_t size) {
    std::scoped_lock lock{invalidation_mutex};
    InvalidatePagesInRegion(addr, size);
    RemovePendingShaders();
}

void ShaderCache::InvalidateRegions(std::span<const std::pair<DAddr, size_t>> sequences) {
    if (sequences.empty()) {
        return;
    }
    std::scoped_lock lock{invalidation_mutex};
    for (const auto& [addr, size] : sequences) {
        InvalidatePagesInRegion(addr, size);
    }
    RemovePendingShaders();
}

void ShaderCache::OnCacheInvalidation(VAddr addr, size_t size) {
    std::scoped_lock lock{invalidation_mutex};
    InvalidatePagesInRegion(addr, size);
}

void ShaderCache::SyncGuestHost() {
    std::scoped_lock lock{invalidation_mutex};
    RemovePendingShaders();
}

namespace {

std::vector<u8> ReadProgramBytes(Tegra::MemoryManager& gpu_memory, GPUVAddr address, size_t size) {
    std::vector<u8> bytes(size);
    gpu_memory.ReadBlockUnsafe(address, bytes.data(), bytes.size());
    return bytes;
}

// Capture at the live shader binding, not just on a cold native lowering. PTC, host shader
// caches and repeated pipelines otherwise skip the raw-capture path entirely. Only active
// Ampere programs are eligible; a low per-program variant cap bounds changing CB1 data.
void CaptureNativeShaderContext(Tegra::MemoryManager& memory, const ShaderInfo* shader,
                                GPUVAddr address, std::string_view stage, bool graphics,
                                bool cb1_enabled, GPUVAddr cb1_address, u32 cb1_size,
                                std::array<u32, 3> mesh_registers = {},
                                std::span<const u8> primitive_header = {}) {
    const char* const dump_directory = std::getenv("EDEN_NX2_SHADER_CONTEXT_DUMP");
    if (!dump_directory || !*dump_directory || !shader || !shader->lowered_native) return;

    const auto& native = *shader->lowered_native;
    constexpr size_t header_size = 128;
    const size_t header_bytes = graphics ? header_size : 0;
    if (native.source_size <= header_bytes || native.source_size > (1 << 20) + header_size ||
        (native.source_size - header_bytes) % 16 != 0) return;

    const auto folder = std::filesystem::path{dump_directory};
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    if (error) {
        LOG_WARNING(Shader, "Cannot create native shader context directory: {}", error.message());
        return;
    }

    const u64 title = Settings::GetCurrentProgramID();
    const auto stem = fmt::format("{:016x}-{}-{:x}", native.native_program_key,
                                  graphics ? "gfx" : "cs", address);
    const auto code_path = folder / (stem + ".bin");
    // All file checks and writes are serialized so another shader worker cannot create
    // a partially written .bin or metadata file for this same program.
    static std::mutex capture_mutex;
    static std::unordered_map<std::string, u32> variant_counts;
    std::scoped_lock lock{capture_mutex};

    if (!std::filesystem::exists(code_path) &&
        memory.IsFullyMappedRange(address, native.source_size)) {
        std::vector<u8> source(native.source_size);
        memory.ReadBlock(address, source.data(), source.size());
        {
            std::ofstream code{code_path, std::ios::binary};
            code.write(reinterpret_cast<const char*>(source.data() + header_bytes),
                       source.size() - header_bytes);
        }
        if (graphics) {
            std::ofstream header{folder / (stem + ".header.bin"), std::ios::binary};
            header.write(reinterpret_cast<const char*>(source.data()), header_size);
        }
        std::ofstream metadata{folder / (stem + ".json")};
        metadata << fmt::format(
            "{{\n  \"schema\": \"lichii-native-ampere-v1\",\n"
            "  \"program_key\": \"{:016x}\",\n"
            "  \"title_id\": \"{:016x}\",\n"
            "  \"stage\": \"{}\",\n"
            "  \"program_base\": \"0x{:x}\",\n"
            "  \"native_code_address\": \"0x{:x}\",\n"
            "  \"header_size\": {},\n"
            "  \"instruction_count\": {},\n"
            "  \"native_size\": {}\n}}\n",
            native.native_program_key, title, stage, address, address + header_bytes,
            header_bytes, (native.source_size - header_bytes) / 16,
            native.source_size - header_bytes);
    }

    // Only valid, mapped CB1 data is treated as recorded runtime constants. A missing
    // binding still receives an explicit context record, never a zero-filled false proof.
    const u32 bytes = (std::min)(cb1_size, u32{0x10000});
    const bool mapped = cb1_enabled && bytes != 0 &&
                        cb1_address <= std::numeric_limits<GPUVAddr>::max() - bytes &&
                        memory.IsFullyMappedRange(cb1_address, bytes);
    std::vector<u8> cb1;
    if (mapped) {
        cb1.resize(bytes);
        memory.ReadBlock(cb1_address, cb1.data(), cb1.size());
    }
    const u64 cb1_hash = cb1.empty() ? 0 :
        Common::CityHash64(reinterpret_cast<const char*>(cb1.data()), cb1.size());
    const auto context_group = fmt::format("{}-{:016x}-{}", stem, title, stage);
    const auto variant = fmt::format("{}-{:016x}", context_group, cb1_hash);
    const auto context_path = folder / (variant + ".context.json");
    if (std::filesystem::exists(context_path) || variant_counts[context_group] >= 8) return;
    // Do not count an unbound or unmapped state against the eight valid CB1 variants.
    if (mapped) ++variant_counts[context_group];
    const auto cb1_filename = variant + ".cb1.bin";
    if (mapped) {
        std::ofstream out{folder / cb1_filename, std::ios::binary};
        out.write(reinterpret_cast<const char*>(cb1.data()), cb1.size());
        if (!out) {
            LOG_WARNING(Shader, "Cannot write CB1 runtime capture {}", cb1_filename);
            return;
        }
    }
    const char* cb1_status = !cb1_enabled ? "unbound" : bytes == 0 ? "empty" :
                             mapped ? "captured" : "unmapped";
    const auto primitive_filename = variant + ".primitive-header.bin";
    if (!primitive_header.empty()) {
        std::ofstream out{folder / primitive_filename, std::ios::binary};
        out.write(reinterpret_cast<const char*>(primitive_header.data()), primitive_header.size());
    }
    std::ofstream context{context_path};
    context << fmt::format(
        "{{\n  \"schema\": \"lichii-native-ampere-runtime-v1\",\n"
        "  \"source_bin\": \"{}.bin\",\n"
        "  \"title_id\": \"{:016x}\",\n"
        "  \"stage\": \"{}\",\n"
        "  \"program_key\": \"{:016x}\",\n"
        "  \"program_base\": \"0x{:x}\",\n"
        "  \"cb1_status\": \"{}\",\n"
        "  \"cb1_gpu_address\": \"0x{:x}\",\n"
        "  \"cb1_declared_bytes\": {},\n"
        "  \"cb1_captured_bytes\": {},\n"
        "  \"cb1_file\": \"{}\",\n"
        "  \"mesh_shader_a\": {},\n"
        "  \"mesh_shader_b\": {},\n"
        "  \"mesh_init_shader\": {},\n"
        "  \"primitive_header_file\": \"{}\"\n}}\n",
        stem, title, stage, native.native_program_key, address, cb1_status,
        cb1_address, cb1_size, cb1.size(), mapped ? cb1_filename : "",
        mesh_registers[0], mesh_registers[1], mesh_registers[2],
        primitive_header.empty() ? "" : primitive_filename);
}

} // Anonymous namespace

std::vector<CapturedProgram> ShaderCache::CaptureGraphicsPrograms(
    const std::array<u64, NUM_PROGRAMS>& unique_hashes) const {
    constexpr size_t native_header_size = 128;
    std::vector<CapturedProgram> programs;
    const auto& regs{maxwell3d->regs};
    const bool native = maxwell3d->GetNativeMacroEngine().native_turing;
    for (size_t index = 0; index < NUM_PROGRAMS; ++index) {
        const ShaderInfo* const info = shader_infos[index];
        if (unique_hashes[index] == 0 || !info || info->unique_hash != unique_hashes[index]) {
            continue;
        }
        const auto& config{regs.pipelines[index]};
        const GPUVAddr address{native ? (static_cast<u64>(config.reserved[0] & 0xff) << 32) |
                                            config.reserved[1]
                                      : regs.program_region.Address() + config.offset};
        CapturedProgram program{
            .slot = static_cast<u32>(index), .unique_hash = info->unique_hash, .address = address};
        if (const auto& lowered = info->lowered_native) {
            const size_t size = lowered->source_size;
            program.native_key = lowered->native_program_key;
            program.header =
                ReadProgramBytes(*gpu_memory, address, (std::min)(size, native_header_size));
            if (size > native_header_size) {
                program.code = ReadProgramBytes(*gpu_memory, address + native_header_size,
                                                size - native_header_size);
            }
        } else {
            program.code = ReadProgramBytes(*gpu_memory, address, info->size_bytes);
        }
        programs.push_back(std::move(program));
    }
    return programs;
}

std::optional<CapturedProgram> ShaderCache::CaptureComputeProgram() const {
    const GPUVAddr address{kepler_compute->regs.code_loc.Address() +
                           kepler_compute->launch_description.program_start};
    const std::optional<VAddr> cpu_address{gpu_memory->GpuToCpuAddress(address)};
    const ShaderInfo* const info = cpu_address ? TryGet(*cpu_address) : nullptr;
    if (!info) {
        return std::nullopt;
    }
    const auto& lowered = info->lowered_native;
    return CapturedProgram{
        .slot = static_cast<u32>(NUM_PROGRAMS),
        .unique_hash = info->unique_hash,
        .native_key = lowered ? lowered->native_program_key : 0,
        .address = address,
        .header = {},
        .code = ReadProgramBytes(*gpu_memory, address,
                                 lowered ? lowered->source_size : info->size_bytes),
    };
}

void ShaderCache::RefreshDebugRejections(bool unrejected) {
    std::vector<std::pair<VAddr, VAddr>> ranges;
    {
        std::scoped_lock lock{lookup_mutex};
        for (const auto& [addr, entry] : lookup_cache) {
            const ShaderInfo* const info = entry->data;
            if (info->lowered_native &&
                (VideoCore::Nx2Debug::IsRejected(info->unique_hash) ||
                 VideoCore::Nx2Debug::IsRejected(info->lowered_native->native_program_key))) {
                ranges.emplace_back(entry->addr_start, entry->addr_end);
            }
        }
    }
    {
        std::scoped_lock lock{invalidation_mutex};
        for (const auto& [start, end] : ranges) {
            InvalidatePagesInRegion(start, end - start);
        }
        RemovePendingShaders();
    }
    if (unrejected) {
        std::scoped_lock lock{rejected_mutex};
        rejected_native.clear();
    }
    // shader_infos may point at removed programs: every channel must refresh its stages.
    for (const size_t id : active_channel_ids) {
        channel_storage[id].maxwell3d.dirty.flags[VideoCommon::Dirty::Shaders] = true;
    }
    LOG_INFO(HW_GPU, "Debug server rejections applied: {} cached programs dropped{}", ranges.size(),
             unrejected ? ", earlier rejections forgotten" : "");
}

ShaderCache::ShaderCache(Tegra::MaxwellDeviceMemoryManager& device_memory_)
    : device_memory{device_memory_} {}

// Task and mesh programs are translated with the mesh registers and the header of the pass-through
// geometry program (per-primitive outputs), so pipelines are keyed by them too.
u64 ShaderCache::MeshStateHash() const {
    using Regs = Tegra::Engines::Maxwell3D::Regs;
    const auto& regs{maxwell3d->regs};
    std::array<u32, 3 + 20> state{regs.reg_array[Regs::MeshShaderA], regs.reg_array[Regs::MeshShaderB],
                                  regs.reg_array[Regs::MeshInitShader]};
    if (regs.IsShaderConfigEnabled(Regs::ShaderType::Geometry)) {
        const auto& geometry{regs.pipelines[static_cast<size_t>(Regs::ShaderType::Geometry)]};
        const GPUVAddr address{(static_cast<u64>(geometry.reserved[0] & 0xff) << 32) |
                               geometry.reserved[1]};
        gpu_memory->ReadBlockUnsafe(address, state.data() + 3, 20 * sizeof(u32));
    }
    return Common::CityHash64(reinterpret_cast<const char*>(state.data()), sizeof(state)) | 1;
}

bool ShaderCache::RefreshStages(std::array<u64, 6>& unique_hashes) try {
    auto& dirty{maxwell3d->dirty.flags};
    const u64 removals{removal_serial.load(std::memory_order_acquire)};
    if (!dirty[VideoCommon::Dirty::Shaders] && removals == seen_removal_serial) {
        return last_shaders_valid;
    }
    seen_removal_serial = removals;
    dirty[VideoCommon::Dirty::Shaders] = false;

    const GPUVAddr base_addr{maxwell3d->regs.program_region.Address()};
    const bool native = maxwell3d->GetNativeMacroEngine().native_turing;
    const bool mesh{native && maxwell3d->regs.IsMeshEnabled()};
    for (size_t index = 0; index < Tegra::Engines::Maxwell3D::Regs::MaxShaderProgram; ++index) {
        if (!maxwell3d->regs.IsShaderConfigEnabled(index)) {
            unique_hashes[index] = 0;
            continue;
        }
        const auto& shader_config{maxwell3d->regs.pipelines[index]};
        const auto program{static_cast<Tegra::Engines::Maxwell3D::Regs::ShaderType>(index)};
        if (program == Tegra::Engines::Maxwell3D::Regs::ShaderType::Pixel &&
            !maxwell3d->regs.rasterize_enable) {
            unique_hashes[index] = 0;
            continue;
        }
        if (mesh && program == Tegra::Engines::Maxwell3D::Regs::ShaderType::Geometry) {
            // A header without code: the mesh program's environment reads its output map.
            unique_hashes[index] = 0;
            continue;
        }
        const GPUVAddr shader_addr{native
            ? (static_cast<u64>(shader_config.reserved[0] & 0xff) << 32) | shader_config.reserved[1]
            : base_addr + shader_config.offset};
        const std::optional<VAddr> cpu_shader_addr{gpu_memory->GpuToCpuAddress(shader_addr)};
        if (!cpu_shader_addr) {
            LOG_ERROR(HW_GPU, "Invalid GPU address for shader {:#016x}", shader_addr);
            last_shaders_valid = false;
            return false;
        }
        const ShaderInfo* shader_info{TryGet(*cpu_shader_addr)};
        if (!shader_info && native && IsRejected(*cpu_shader_addr)) {
            // A rejected program fails the same way every time it is bound: skip its draws
            // without rethrowing (two C++ throws each time cost ~1-2% of the GPU thread).
            maxwell3d->GetNativeMacroEngine().native_execution_failed = true;
            last_shaders_valid = false;
            return false;
        }
        if (!shader_info) {
            const u32 start_address{native ? 0 : shader_config.offset};
            try {
                // Native programs are lowered while the environment is built (once: the
                // ShaderInfo keeps the result for the pipeline's environments).
                RethrowIfRejected(*cpu_shader_addr);
                const Nx2Profile::Scope profile_scope{Nx2Profile::Zone::ShaderCapture};
                GraphicsEnvironment env{*maxwell3d, *gpu_memory, program, native ? shader_addr : base_addr, start_address,
                                        {}, shader_cache_v2};
                shader_info = MakeShaderInfo(env, *cpu_shader_addr);
            } catch (const Shader::Exception&) {
                RememberRejection(*cpu_shader_addr);
                // Once per program: which stage slot it is bound to, its header and the slot's
                // registers (to tell how a pipeline with task/mesh programs is set up).
                static std::unordered_set<GPUVAddr> described;
                if (native && described.insert(shader_addr).second) {
                    std::array<u32, 5> header{};
                    gpu_memory->ReadBlockUnsafe(shader_addr, header.data(), sizeof(header));
                    std::array<u32, sizeof(shader_config) / sizeof(u32)> config{};
                    std::memcpy(config.data(), &shader_config, sizeof(config));
                    LOG_ERROR(HW_GPU, "Rejected program {:#x} is bound to stage {}: header {:08x}, slot {:08x}",
                              shader_addr, index, fmt::join(header, " "), fmt::join(config, " "));
                }
                if (native && std::getenv("EDEN_NX2_SHADER_DUMP")) {
                    for (size_t next = index + 1; next < unique_hashes.size(); ++next) {
                        const auto next_program =
                            static_cast<Tegra::Engines::Maxwell3D::Regs::ShaderType>(next);
                        if (!maxwell3d->regs.IsShaderConfigEnabled(next) ||
                            (mesh && next_program == Tegra::Engines::Maxwell3D::Regs::ShaderType::Geometry) ||
                            (!maxwell3d->regs.rasterize_enable &&
                             next_program == Tegra::Engines::Maxwell3D::Regs::ShaderType::Pixel)) {
                            continue;
                        }
                        const auto& next_config = maxwell3d->regs.pipelines[next];
                        const GPUVAddr next_address =
                            (static_cast<u64>(next_config.reserved[0] & 0xff) << 32) |
                            next_config.reserved[1];
                        if (!gpu_memory->IsFullyMappedRange(next_address, 128)) {
                            LOG_WARNING(HW_GPU, "Downstream native stage {} at {:#x} is unmapped",
                                        next, next_address);
                            continue;
                        }
                        try {
                            GraphicsEnvironment capture{*maxwell3d, *gpu_memory, next_program,
                                                        next_address, 0};
                            LOG_INFO(HW_GPU, "Preflighted downstream native stage {} at {:#x}",
                                     next, next_address);
                        } catch (const std::exception& error) {
                            LOG_ERROR(HW_GPU, "Downstream native stage {} at {:#x} rejected: {}",
                                      next, next_address, error.what());
                        }
                    }
                }
                throw;
            }
        }
        if (native && shader_info->lowered_native &&
            std::getenv("EDEN_NX2_SHADER_CONTEXT_DUMP")) {
            const size_t binding_stage = program == Tegra::Engines::Maxwell3D::Regs::ShaderType::Pixel ? 4 :
                                         program == Tegra::Engines::Maxwell3D::Regs::ShaderType::Geometry ? 3 :
                                         program == Tegra::Engines::Maxwell3D::Regs::ShaderType::Tessellation ? 2 :
                                         program == Tegra::Engines::Maxwell3D::Regs::ShaderType::TessellationInit ? 1 : 0;
            const auto& cb1 = maxwell3d->state.shader_stages[binding_stage].const_buffers[1];
            const bool task = mesh && program == Tegra::Engines::Maxwell3D::Regs::ShaderType::VertexB &&
                              maxwell3d->regs.IsShaderConfigEnabled(
                                  Tegra::Engines::Maxwell3D::Regs::ShaderType::Tessellation);
            const bool mesh_stage = mesh &&
                (program == Tegra::Engines::Maxwell3D::Regs::ShaderType::VertexB ||
                 program == Tegra::Engines::Maxwell3D::Regs::ShaderType::Tessellation) && !task;
            const std::string_view stage = task ? "ts" : mesh_stage ? "ms" :
                program == Tegra::Engines::Maxwell3D::Regs::ShaderType::VertexB ? "vs" :
                program == Tegra::Engines::Maxwell3D::Regs::ShaderType::TessellationInit ? "tc" :
                program == Tegra::Engines::Maxwell3D::Regs::ShaderType::Tessellation ? "te" :
                program == Tegra::Engines::Maxwell3D::Regs::ShaderType::Geometry ? "gs" : "fs";
            std::array<u32, 3> mesh_regs{};
            std::array<u8, 80> primitive_header{};
            std::span<const u8> primitive{};
            if (task || mesh_stage) {
                const auto& regs = maxwell3d->regs;
                mesh_regs = {regs.reg_array[Tegra::Engines::Maxwell3D::Regs::MeshShaderA],
                             regs.reg_array[Tegra::Engines::Maxwell3D::Regs::MeshShaderB],
                             regs.reg_array[Tegra::Engines::Maxwell3D::Regs::MeshInitShader]};
                if (mesh_stage && regs.IsShaderConfigEnabled(
                        Tegra::Engines::Maxwell3D::Regs::ShaderType::Geometry)) {
                    const auto& geom = regs.pipelines[static_cast<size_t>(
                        Tegra::Engines::Maxwell3D::Regs::ShaderType::Geometry)];
                    const GPUVAddr primitive_address =
                        (static_cast<u64>(geom.reserved[0] & 0xff) << 32) | geom.reserved[1];
                    if (gpu_memory->IsFullyMappedRange(primitive_address, primitive_header.size())) {
                        gpu_memory->ReadBlock(primitive_address, primitive_header.data(), primitive_header.size());
                        primitive = primitive_header;
                    }
                }
            }
            CaptureNativeShaderContext(*gpu_memory, shader_info, shader_addr, stage, true,
                                       cb1.enabled, cb1.address, cb1.size, mesh_regs, primitive);
        }
        shader_infos[index] = shader_info;
        unique_hashes[index] = shader_info->unique_hash;
        if (mesh && (program == Tegra::Engines::Maxwell3D::Regs::ShaderType::VertexB ||
                     program == Tegra::Engines::Maxwell3D::Regs::ShaderType::Tessellation)) {
            unique_hashes[index] ^= MeshStateHash();
        }
    }
    last_shaders_valid = true;
    return true;
} catch (const Shader::Exception& error) {
    auto& native = maxwell3d->GetNativeMacroEngine();
    if (!native.native_turing) throw;
    native.native_execution_failed = true;
    last_shaders_valid = false;
    // Each rejected program is logged once (the draws that use it keep failing quietly).
    static std::unordered_set<std::string> logged;
    if (logged.insert(error.what()).second) {
        LOG_ERROR(HW_GPU, "Native graphics shader rejected: {}", error.what());
    }
    // Once: show what the vertex stage has bound, to find where an unbound bank's data lives.
    static std::vector<std::string> dumped_errors;
    if (const std::string what{error.what()};
        what.find("constant buffer") != std::string::npos && dumped_errors.size() < 12 &&
        std::count(dumped_errors.begin(), dumped_errors.end(), what) < 3) {
        dumped_errors.push_back(what);
        LOG_INFO(HW_GPU, "VS cbuf dump for: {}", what.substr(0, 40));
        for (u32 slot = 3; slot < 8; ++slot) {
            const auto& buffer = maxwell3d->state.shader_stages[0].const_buffers[slot];
            if (!buffer.enabled) continue;
            std::array<u32, 24> words{};
            gpu_memory->ReadBlockUnsafe(buffer.address, words.data(), sizeof(words));
            LOG_INFO(HW_GPU, "VS cbuf {} at {:#x} size {:#x}: {:08x}", slot, buffer.address, buffer.size,
                     fmt::join(words, " "));
        }
    }
    return false;
}

const ShaderInfo* ShaderCache::ComputeShader() {
    const GPUVAddr program_base{kepler_compute->regs.code_loc.Address()};
    const auto& qmd{kepler_compute->launch_description};
    const GPUVAddr shader_addr{program_base + qmd.program_start};
    const std::optional<VAddr> cpu_shader_addr{gpu_memory->GpuToCpuAddress(shader_addr)};
    if (!cpu_shader_addr) {
        LOG_ERROR(HW_GPU, "Invalid GPU address for shader {:#016x}", shader_addr);
        return nullptr;
    }
    const ShaderInfo* shader = TryGet(*cpu_shader_addr);
    if (shader) {
        if (shader->lowered_native &&
            !Shader::Ampere::VerifyTrapGuards(shader->lowered_native->trap_prefix,
                                             [&](u32 bank, u32 offset) {
                                                 return ReadNativeGuardConstant(
                                                     *kepler_compute, *gpu_memory, bank, offset);
                                             }))
            throw Shader::Ampere::TrapGuardError{};
    } else if (kepler_compute->IsNativeMode() && IsRejected(*cpu_shader_addr)) {
        // A rejected program fails the same way on every dispatch. GetGraphicsEnvironments
        // skips those draws without rethrowing; doing the same here avoids two C++ throws, a
        // mutex-guarded insert and an exception_ptr for each dispatch of the program. The
        // caller already turns a missing shader into a failed dispatch.
        return nullptr;
    } else {
        try {
            RethrowIfRejected(*cpu_shader_addr);
            const Nx2Profile::Scope profile_scope{Nx2Profile::Zone::ShaderCapture};
            ComputeEnvironment env{*kepler_compute, *gpu_memory, program_base, qmd.program_start, {},
                                   shader_cache_v2};
            shader = MakeShaderInfo(env, *cpu_shader_addr);
        } catch (const Shader::Ampere::TrapGuardError&) {
            // Constants can become valid on the next dispatch; do not cache this rejection.
            throw;
        } catch (const Shader::Exception&) {
            RememberRejection(*cpu_shader_addr);
            throw;
        }
    }
    if (kepler_compute->IsNativeMode() && shader->lowered_native &&
        std::getenv("EDEN_NX2_SHADER_CONTEXT_DUMP")) {
        const auto& qmd = kepler_compute->launch_description;
        if (qmd.const_buffer_config.size() > 1) {
            const auto& cb1 = qmd.const_buffer_config[1];
            CaptureNativeShaderContext(*gpu_memory, shader, shader_addr, "cs", false,
                                       (qmd.const_buffer_enable_mask.Value() & (1u << 1)) != 0,
                                       cb1.Address(), cb1.size.Value());
        }
    }
    // Check both warm and cold shader paths, before selecting a disk-cached pipeline.
    // EDEN_NX2_DLSS_REPLACEMENT=native retains the native network for comparisons.
    if (kepler_compute->IsNativeMode() && shader->lowered_native &&
        DlssContract::IsFastFusionCandidate(program_id,
                                            shader->lowered_native->native_program_key)) {
        kepler_compute->TraceDlssContract(shader->lowered_native->native_program_key);
    }
    if (kepler_compute->IsNativeMode() && shader->lowered_native &&
        kepler_compute->BypassOotNetwork(program_id, shader->lowered_native->native_program_key)) {
        throw FireEmblemUpscale::DispatchReplaced{};
    }
    if (kepler_compute->IsNativeMode() && shader->lowered_native &&
        kepler_compute->ProcessGameDlss(program_id, shader->lowered_native->native_program_key)) {
        throw FireEmblemUpscale::DispatchReplaced{};
    }
    return shader;
}

const ShaderInfo* ShaderCache::KnownComputeShader() const {
    const auto& qmd{kepler_compute->launch_description};
    const std::optional<VAddr> cpu_shader_addr{
        gpu_memory->GpuToCpuAddress(kepler_compute->regs.code_loc.Address() + qmd.program_start)};
    return cpu_shader_addr ? TryGet(*cpu_shader_addr) : nullptr;
}

bool ShaderCache::KnownComputeShaderChecks(const ShaderInfo* shader) {
    if (!shader->lowered_native) {
        return true;
    }
    const u64 key = shader->lowered_native->native_program_key;
    if (kepler_compute->IsNativeMode()) {
        using namespace OotNetwork;
        const bool network_pass =
            BypassEnabled(program_id) &&
            (key == PrepassProgramKey || key == UnusedDispatchProgramKey ||
             key == NetworkProgramKey || key == OutputProgramKey ||
             std::ranges::find(BaseOutputPrograms, key, &BaseOutputProgram::key) !=
                 BaseOutputPrograms.end());
        if (network_pass || DlssContract::IsFastFusionCandidate(program_id, key) ||
            FireEmblemUpscale::CapturedPasses(program_id)) {
            return false;
        }
    }
    if (!Shader::Ampere::VerifyTrapGuards(
            shader->lowered_native->trap_prefix, [&](u32 bank, u32 offset) {
                return ReadNativeGuardConstant(*kepler_compute, *gpu_memory, bank, offset);
            })) {
        throw Shader::Ampere::TrapGuardError{};
    }
    // Any other program: the bypass notes the first one after the network, nothing else.
    if (kepler_compute->IsNativeMode() && kepler_compute->BypassOotNetwork(program_id, key)) {
        throw FireEmblemUpscale::DispatchReplaced{};
    }
    return true;
}

void ShaderCache::RethrowIfRejected(VAddr addr) {
    std::scoped_lock lock{rejected_mutex};
    if (const auto it = rejected_native.find(addr); it != rejected_native.end()) {
        std::rethrow_exception(it->second);
    }
}

bool ShaderCache::IsRejected(VAddr addr) {
    std::scoped_lock lock{rejected_mutex};
    return rejected_native.contains(addr);
}

void ShaderCache::RememberRejection(VAddr addr) {
    std::scoped_lock lock{rejected_mutex};
    if (rejected_native.contains(addr)) {
        // Called from a catch block: keep the first exception for that address instead of
        // capturing and inserting a new one on every rejected dispatch.
        return;
    }
    rejected_native.emplace(addr, std::current_exception());
    page_filter.Mark(addr, 1); // InnerInvalidation reaches InvalidatePagesInRegion for it
    rejected_lowest.store((std::min)(rejected_lowest.load(std::memory_order_relaxed), addr),
                          std::memory_order_relaxed);
    rejected_highest.store((std::max)(rejected_highest.load(std::memory_order_relaxed), addr),
                           std::memory_order_relaxed);
}

void ShaderCache::GetGraphicsEnvironments(GraphicsEnvironments& result,
                                          const std::array<u64, NUM_PROGRAMS>& unique_hashes) {
    size_t env_index{};
    const GPUVAddr base_addr{maxwell3d->regs.program_region.Address()};
    const bool native = maxwell3d->GetNativeMacroEngine().native_turing;
    for (size_t index = 0; index < NUM_PROGRAMS; ++index) {
        if (unique_hashes[index] == 0) {
            continue;
        }
        const auto program{static_cast<Tegra::Engines::Maxwell3D::Regs::ShaderType>(index)};
        auto& env{result.envs[index]};
        const auto& config = maxwell3d->regs.pipelines[index];
        const u32 start_address{native ? 0 : config.offset};
        const GPUVAddr address = native
            ? (static_cast<u64>(config.reserved[0] & 0xff) << 32) | config.reserved[1]
            : base_addr;
        env = GraphicsEnvironment{*maxwell3d, *gpu_memory, program, address, start_address,
                                  shader_infos[index]->lowered_native};
        env.SetCachedSize(shader_infos[index]->size_bytes);
        result.env_ptrs[env_index++] = &env;
    }
}

ShaderInfo* ShaderCache::TryGet(VAddr addr) const {
    std::scoped_lock lock{lookup_mutex};

    const auto it = lookup_cache.find(addr);
    if (it == lookup_cache.end()) {
        return nullptr;
    }
    return it->second->data;
}

void ShaderCache::Register(std::unique_ptr<ShaderInfo> data, VAddr addr, size_t size) {
    std::scoped_lock lock{invalidation_mutex, lookup_mutex};

    const VAddr addr_end = addr + size;
    Entry* const entry = NewEntry(addr, addr_end, data.get());
    page_filter.Mark(addr, size);

    const u64 page_end = (addr_end + LICHII_PAGESIZE - 1) >> LICHII_PAGEBITS;
    for (u64 page = addr >> LICHII_PAGEBITS; page < page_end; ++page) {
        invalidation_cache[page].push_back(entry);
    }

    storage.push_back(std::move(data));

    device_memory.UpdatePagesCachedCount(addr, size, 1);
}

void ShaderCache::InvalidatePagesInRegion(VAddr addr, size_t size) {
    const VAddr addr_end = addr + size;
    // Rejected programs are few and in a few places: look at them only for writes that can
    // reach them (this runs for every inline upload and pre-draw invalidation).
    if (addr_end > rejected_lowest.load(std::memory_order_relaxed) &&
        addr <= rejected_highest.load(std::memory_order_relaxed)) {
        std::scoped_lock lock{rejected_mutex};
        boost::unordered::erase_if(rejected_native, [&](const auto& rejected) {
            return rejected.first >= addr && rejected.first < addr_end;
        });
    }
    const u64 page_end = (addr_end + LICHII_PAGESIZE - 1) >> LICHII_PAGEBITS;
    for (u64 page = addr >> LICHII_PAGEBITS; page < page_end; ++page) {
        auto it = invalidation_cache.find(page);
        if (it == invalidation_cache.end()) {
            continue;
        }
        InvalidatePageEntries(it->second, addr, addr_end);
    }
}

void ShaderCache::RemovePendingShaders() {
    if (marked_for_removal.empty()) {
        return;
    }
    // Remove duplicates
    std::ranges::sort(marked_for_removal);
    marked_for_removal.erase(std::unique(marked_for_removal.begin(), marked_for_removal.end()),
                             marked_for_removal.end());
    // Linear growth anyways - maybe consider static_vector instead?
    boost::container::small_vector<ShaderInfo*, 16> removed_shaders;
    std::scoped_lock lock{lookup_mutex};
    for (Entry* const entry : marked_for_removal) {
        removed_shaders.push_back(entry->data);
        auto const it = lookup_cache.find(entry->addr_start);
        ASSERT(it != lookup_cache.end());
        lookup_cache.erase(it);
    }
    marked_for_removal.clear();

    if (!removed_shaders.empty()) {
        Nx2FastDraw::Break(Nx2FastDraw::Reason::Invalidation);
        removal_serial.fetch_add(1, std::memory_order_release);
        if (removed_shaders.size() > 16) {
            std::ranges::sort(removed_shaders, std::less<ShaderInfo*>{});
            std::erase_if(storage, [&removed_shaders](const std::unique_ptr<ShaderInfo>& shader) {
                return std::binary_search(removed_shaders.begin(), removed_shaders.end(),
                                          shader.get(), std::less<ShaderInfo*>{});
            });
        } else {
            std::erase_if(storage, [&removed_shaders](const std::unique_ptr<ShaderInfo>& shader) {
                return std::ranges::find(removed_shaders, shader.get()) != removed_shaders.end();
            });
        }
    }
}

void ShaderCache::InvalidatePageEntries(std::vector<Entry*>& entries, VAddr addr, VAddr addr_end) {
    size_t index = 0;
    while (index < entries.size()) {
        Entry* const entry = entries[index];
        if (!entry->Overlaps(addr, addr_end)) {
            ++index;
            continue;
        }

        UnmarkMemory(entry);
        RemoveEntryFromInvalidationCache(entry);
        marked_for_removal.push_back(entry);
    }
}

void ShaderCache::RemoveEntryFromInvalidationCache(const Entry* entry) {
    const u64 page_end = (entry->addr_end + LICHII_PAGESIZE - 1) >> LICHII_PAGEBITS;
    for (u64 page = entry->addr_start >> LICHII_PAGEBITS; page < page_end; ++page) {
        const auto entries_it = invalidation_cache.find(page);
        ASSERT(entries_it != invalidation_cache.end());
        std::vector<Entry*>& entries = entries_it->second;

        const auto entry_it = std::ranges::find(entries, entry);
        ASSERT(entry_it != entries.end());
        entries.erase(entry_it);
    }
}

void ShaderCache::UnmarkMemory(Entry* entry) {
    if (!entry->is_memory_marked) {
        return;
    }
    entry->is_memory_marked = false;

    const VAddr addr = entry->addr_start;
    const size_t size = entry->addr_end - addr;
    device_memory.UpdatePagesCachedCount(addr, size, -1);
}

ShaderCache::Entry* ShaderCache::NewEntry(VAddr addr, VAddr addr_end, ShaderInfo* data) {
    auto entry = std::make_unique<Entry>(Entry{addr, addr_end, data});
    Entry* const entry_pointer = entry.get();

    lookup_cache.emplace(addr, std::move(entry));
    return entry_pointer;
}

const ShaderInfo* ShaderCache::MakeShaderInfo(GenericEnvironment& env, VAddr cpu_addr) {
    auto info = std::make_unique<ShaderInfo>();
    if (const std::optional<u64> cached_hash{env.Analyze()}) {
        info->unique_hash = *cached_hash;
        info->size_bytes = env.CachedSizeBytes();
    } else {
        // Slow path, not really hit on commercial games
        // Build a control flow graph to get the real shader size
        Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_block;
        Shader::Maxwell::Flow::CFG cfg{env, flow_block, env.StartAddress()};
        info->unique_hash = env.CalculateHash();
        info->size_bytes = env.ReadSizeBytes();
    }
    info->lowered_native = env.LoweredNative();
    if (const auto record = env.TakeProgramRecord(); record && shader_cache_v2) {
        shader_cache_v2->RecordProgram(*record);
    }
    const size_t size_bytes{env.SourceSizeBytes() ? env.SourceSizeBytes() : info->size_bytes};
    const ShaderInfo* const result{info.get()};
    Register(std::move(info), cpu_addr, size_bytes);
    return result;
}

} // namespace VideoCommon
