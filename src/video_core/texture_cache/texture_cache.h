// Copyright 2019 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <algorithm>
#include <array>
#include <memory>
#include <mutex>
#include <set>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <boost/icl/interval_map.hpp>
#include <boost/range/iterator_range.hpp>

#include "common/assert.h"
#include "common/common_types.h"
#include "common/math_util.h"
#include "core/core.h"
#include "core/memory.h"
#include "core/settings.h"
#include "video_core/dirty_flags.h"
#include "video_core/engines/fermi_2d.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/gpu.h"
#include "video_core/memory_manager.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/surface.h"
#include "video_core/texture_cache/copy_params.h"
#include "video_core/texture_cache/format_lookup_table.h"
#include "video_core/texture_cache/surface_base.h"
#include "video_core/texture_cache/surface_params.h"
#include "video_core/texture_cache/surface_view.h"

namespace Tegra::Texture {
struct FullTextureInfo;
}

namespace VideoCore {
class RasterizerInterface;
}

namespace VideoCommon {

using VideoCore::Surface::PixelFormat;

using VideoCore::Surface::SurfaceTarget;
using RenderTargetConfig = Tegra::Engines::Maxwell3D::Regs::RenderTargetConfig;

template <typename TSurface, typename TView>
class TextureCache {

public:
    void InvalidateRegion(VAddr addr, std::size_t size) {
        std::lock_guard lock{mutex};

        for (const auto& surface : GetSurfacesInRegion(addr, size)) {
            Unregister(surface);
        }
    }

    /**
     * Guarantees that rendertargets don't unregister themselves if the
     * collide. Protection is currently only done on 3D slices.
     */
    void GuardRenderTargets(bool new_guard) {
        guard_render_targets = new_guard;
    }

    void GuardSamplers(bool new_guard) {
        guard_samplers = new_guard;
    }

    void FlushRegion(VAddr addr, std::size_t size) {
        std::lock_guard lock{mutex};

        auto surfaces = GetSurfacesInRegion(addr, size);
        if (surfaces.empty()) {
            return;
        }
        std::sort(surfaces.begin(), surfaces.end(), [](const TSurface& a, const TSurface& b) {
            return a->GetModificationTick() < b->GetModificationTick();
        });
        for (const auto& surface : surfaces) {
            FlushSurface(surface);
        }
    }

    TView GetTextureSurface(const Tegra::Texture::TICEntry& tic,
                            const VideoCommon::Shader::Sampler& entry) {
        std::lock_guard lock{mutex};
        const auto gpu_addr{tic.Address()};
        if (!gpu_addr) {
            return GetNullSurface(SurfaceParams::ExpectedTarget(entry));
        }

        const std::optional<VAddr> cpu_addr =
            system.GPU().MemoryManager().GpuToCpuAddress(gpu_addr);
        if (!cpu_addr) {
            return GetNullSurface(SurfaceParams::ExpectedTarget(entry));
        }

        if (!IsTypeCompatible(tic.texture_type, entry)) {
            return GetNullSurface(SurfaceParams::ExpectedTarget(entry));
        }

        const auto params{SurfaceParams::CreateForTexture(format_lookup_table, tic, entry)};
        const auto [surface, view] = GetSurface(gpu_addr, *cpu_addr, params, false);
        if (guard_samplers) {
            sampled_textures.push_back(surface);
        }
        return view;
    }

    TView GetImageSurface(const Tegra::Texture::TICEntry& tic,
                          const VideoCommon::Shader::Image& entry) {
        std::lock_guard lock{mutex};
        const auto gpu_addr{tic.Address()};
        if (!gpu_addr) {
            return GetNullSurface(SurfaceParams::ExpectedTarget(entry));
        }
        const std::optional<VAddr> cpu_addr =
            system.GPU().MemoryManager().GpuToCpuAddress(gpu_addr);
        if (!cpu_addr) {
            return GetNullSurface(SurfaceParams::ExpectedTarget(entry));
        }
        const auto params{SurfaceParams::CreateForImage(format_lookup_table, tic, entry)};
        const auto [surface, view] = GetSurface(gpu_addr, *cpu_addr, params, false);
        if (guard_samplers) {
            sampled_textures.push_back(surface);
        }
        return view;
    }

    bool TextureBarrier() {
        const bool any_rt =
            std::any_of(sampled_textures.begin(), sampled_textures.end(),
                        [](const auto& surface) { return surface->IsRenderTarget(); });
        sampled_textures.clear();
        return any_rt;
    }

    TView GetDepthBufferSurface() {
        std::lock_guard lock{mutex};
        auto& maxwell3d = system.GPU().Maxwell3D();
        if (!maxwell3d.dirty.flags[VideoCommon::Dirty::ZetaBuffer]) {
            return depth_buffer.view;
        }
        maxwell3d.dirty.flags[VideoCommon::Dirty::ZetaBuffer] = false;

        const auto& regs{maxwell3d.regs};
        const auto gpu_addr{regs.zeta.Address()};
        if (!gpu_addr || !regs.zeta_enable) {
            SetEmptyDepthBuffer();
            return {};
        }
        const std::optional<VAddr> cpu_addr =
            system.GPU().MemoryManager().GpuToCpuAddress(gpu_addr);
        if (!cpu_addr) {
            SetEmptyDepthBuffer();
            return {};
        }
        const auto depth_params{SurfaceParams::CreateForDepthBuffer(system)};
        auto surface_view = GetSurface(gpu_addr, *cpu_addr, depth_params, true);
        if (depth_buffer.target)
            depth_buffer.target->MarkAsRenderTarget(false, NO_RT);
        depth_buffer.target = surface_view.first;
        depth_buffer.view = surface_view.second;
        if (depth_buffer.target)
            depth_buffer.target->MarkAsRenderTarget(true, DEPTH_RT);
        return surface_view.second;
    }

    TView GetColorBufferSurface(std::size_t index) {
        std::lock_guard lock{mutex};
        ASSERT(index < Tegra::Engines::Maxwell3D::Regs::NumRenderTargets);
        auto& maxwell3d = system.GPU().Maxwell3D();
        if (!maxwell3d.dirty.flags[VideoCommon::Dirty::ColorBuffer0 + index]) {
            return render_targets[index].view;
        }
        maxwell3d.dirty.flags[VideoCommon::Dirty::ColorBuffer0 + index] = false;

        const auto& regs{maxwell3d.regs};
        if (index >= regs.rt_control.count || regs.rt[index].Address() == 0 ||
            regs.rt[index].format == Tegra::RenderTargetFormat::NONE) {
            SetEmptyColorBuffer(index);
            return {};
        }

        const auto& config{regs.rt[index]};
        const auto gpu_addr{config.Address()};
        if (!gpu_addr) {
            SetEmptyColorBuffer(index);
            return {};
        }

        const std::optional<VAddr> cpu_addr =
            system.GPU().MemoryManager().GpuToCpuAddress(gpu_addr);
        if (!cpu_addr) {
            SetEmptyColorBuffer(index);
            return {};
        }

        auto surface_view = GetSurface(gpu_addr, *cpu_addr,
                                       SurfaceParams::CreateForFramebuffer(system, index), true);
        if (render_targets[index].target)
            render_targets[index].target->MarkAsRenderTarget(false, NO_RT);
        render_targets[index].target = surface_view.first;
        render_targets[index].view = surface_view.second;
        if (render_targets[index].target)
            render_targets[index].target->MarkAsRenderTarget(true, static_cast<u32>(index));
        return surface_view.second;
    }

    void MarkColorBufferInUse(std::size_t index) {
        if (auto& render_target = render_targets[index].target) {
            render_target->MarkAsModified(true, Tick());
        }
    }

    void MarkDepthBufferInUse() {
        if (depth_buffer.target) {
            depth_buffer.target->MarkAsModified(true, Tick());
        }
    }

    void SetEmptyDepthBuffer() {
        if (depth_buffer.target == nullptr) {
            return;
        }
        depth_buffer.target->MarkAsRenderTarget(false, NO_RT);
        depth_buffer.target = nullptr;
        depth_buffer.view = nullptr;
    }

    void SetEmptyColorBuffer(std::size_t index) {
        if (render_targets[index].target == nullptr) {
            return;
        }
        render_targets[index].target->MarkAsRenderTarget(false, NO_RT);
        render_targets[index].target = nullptr;
        render_targets[index].view = nullptr;
    }

    void DoFermiCopy(const Tegra::Engines::Fermi2D::Regs::Surface& src_config,
                     const Tegra::Engines::Fermi2D::Regs::Surface& dst_config,
                     const Tegra::Engines::Fermi2D::Config& copy_config) {
        std::lock_guard lock{mutex};
        SurfaceParams src_params = SurfaceParams::CreateForFermiCopySurface(src_config);
        SurfaceParams dst_params = SurfaceParams::CreateForFermiCopySurface(dst_config);
        const GPUVAddr src_gpu_addr = src_config.Address();
        const GPUVAddr dst_gpu_addr = dst_config.Address();
        DeduceBestBlit(src_params, dst_params, src_gpu_addr, dst_gpu_addr);
        const std::optional<VAddr> dst_cpu_addr =
            system.GPU().MemoryManager().GpuToCpuAddress(dst_gpu_addr);
        const std::optional<VAddr> src_cpu_addr =
            system.GPU().MemoryManager().GpuToCpuAddress(src_gpu_addr);
        std::pair<TSurface, TView> dst_surface =
            GetSurface(dst_gpu_addr, *dst_cpu_addr, dst_params, false);
        std::pair<TSurface, TView> src_surface =
            GetSurface(src_gpu_addr, *src_cpu_addr, src_params, false);
        ImageBlit(src_surface.second, dst_surface.second, copy_config);
        dst_surface.first->MarkAsModified(true, Tick());
    }

    TSurface TryFindFramebufferSurface(VAddr addr) {
        if (!addr) {
            return nullptr;
        }
        const VAddr page = addr >> registry_page_bits;
        std::vector<TSurface>& list = registry[page];
        for (auto& surface : list) {
            if (surface->GetCpuAddr() == addr) {
                return surface;
            }
        }
        return nullptr;
    }

    u64 Tick() {
        return ++ticks;
    }

protected:
    explicit TextureCache(Core::System& system, VideoCore::RasterizerInterface& rasterizer,
                          bool is_astc_supported)
        : system{system}, is_astc_supported{is_astc_supported}, rasterizer{rasterizer} {
        for (std::size_t i = 0; i < Tegra::Engines::Maxwell3D::Regs::NumRenderTargets; i++) {
            SetEmptyColorBuffer(i);
        }

        SetEmptyDepthBuffer();
        staging_cache.SetSize(2);

        const auto make_siblings = [this](PixelFormat a, PixelFormat b) {
            siblings_table[static_cast<std::size_t>(a)] = b;
            siblings_table[static_cast<std::size_t>(b)] = a;
        };
        std::fill(siblings_table.begin(), siblings_table.end(), PixelFormat::Invalid);
        make_siblings(PixelFormat::Z16, PixelFormat::R16U);
        make_siblings(PixelFormat::Z32F, PixelFormat::R32F);
        make_siblings(PixelFormat::Z32FS8, PixelFormat::RG32F);

        sampled_textures.reserve(64);
    }

    ~TextureCache() = default;

    virtual TSurface CreateSurface(GPUVAddr gpu_addr, const SurfaceParams& params) = 0;

    virtual void ImageCopy(TSurface& src_surface, TSurface& dst_surface,
                           const CopyParams& copy_params) = 0;

    virtual void ImageBlit(TView& src_view, TView& dst_view,
                           const Tegra::Engines::Fermi2D::Config& copy_config) = 0;

    // Depending on the backend, a buffer copy can be slow as it means deoptimizing the texture
    // and reading it from a separate buffer.
    virtual void BufferCopy(TSurface& src_surface, TSurface& dst_surface) = 0;

    void ManageRenderTargetUnregister(TSurface& surface) {
        auto& dirty = system.GPU().Maxwell3D().dirty;
        const u32 index = surface->GetRenderTarget();
        if (index == DEPTH_RT) {
            dirty.flags[VideoCommon::Dirty::ZetaBuffer] = true;
        } else {
            dirty.flags[VideoCommon::Dirty::ColorBuffer0 + index] = true;
        }
        dirty.flags[VideoCommon::Dirty::RenderTargets] = true;
    }

    void Register(TSurface surface) {
        const GPUVAddr gpu_addr = surface->GetGpuAddr();
        const std::size_t size = surface->GetSizeInBytes();
        const std::optional<VAddr> cpu_addr =
            system.GPU().MemoryManager().GpuToCpuAddress(gpu_addr);
        if (!cpu_addr) {
            LOG_CRITICAL(HW_GPU, "Failed to register surface with unmapped gpu_address 0x{:016x}",
                         gpu_addr);
            return;
        }
        surface->SetCpuAddr(*cpu_addr);
        RegisterInnerCache(surface);
        surface->MarkAsRegistered(true);
        rasterizer.UpdatePagesCachedCount(*cpu_addr, size, 1);
    }

    void Unregister(TSurface surface) {
        if (guard_render_targets && surface->IsProtected()) {
            return;
        }
        if (!guard_render_targets && surface->IsRenderTarget()) {
            ManageRenderTargetUnregister(surface);
        }
        const std::size_t size = surface->GetSizeInBytes();
        const VAddr cpu_addr = surface->GetCpuAddr();
        rasterizer.UpdatePagesCachedCount(cpu_addr, size, -1);
        UnregisterInnerCache(surface);
        surface->MarkAsRegistered(false);
        ReserveSurface(surface->GetSurfaceParams(), surface);
    }

    TSurface GetUncachedSurface(const GPUVAddr gpu_addr, const SurfaceParams& params) {
        if (const auto surface = TryGetReservedSurface(params); surface) {
            surface->SetGpuAddr(gpu_addr);
            return surface;
        }
        // No reserved surface available, create a new one and reserve it
        auto new_surface{CreateSurface(gpu_addr, params)};
        return new_surface;
    }

    Core::System& system;
    const bool is_astc_supported;

private:
    enum class RecycleStrategy : u32 {
        Ignore = 0,
        Flush = 1,
        BufferCopy = 3,
    };

    enum class DeductionType : u32 {
        DeductionComplete,
        DeductionIncomplete,
        DeductionFailed,
    };

    struct Deduction {
        DeductionType type{DeductionType::DeductionFailed};
        TSurface surface{};

        bool Failed() const {
            return type == DeductionType::DeductionFailed;
        }

        bool Incomplete() const {
            return type == DeductionType::DeductionIncomplete;
        }

        bool IsDepth() const {
            return surface->GetSurfaceParams().IsPixelFormatZeta();
        }
    };

    /**
     * Takes care of selecting a proper strategy to deal with a texture recycle.
     *
     * @param overlaps      The overlapping surfaces registered in the cache.
     * @param params        The parameters on the new surface.
     * @param gpu_addr      The starting address of the new surface.
     * @param untopological Indicates to the recycler that the texture has no way
     *                      to match the overlaps due to topological reasons.
     **/
    RecycleStrategy PickStrategy(std::vector<TSurface>& overlaps, const SurfaceParams& params,
                                 const GPUVAddr gpu_addr, const MatchTopologyResult untopological) {
        if (Settings::values.use_accurate_gpu_emulation) {
            return RecycleStrategy::Flush;
        }
        // 3D Textures decision
        if (params.block_depth > 1 || params.target == SurfaceTarget::Texture3D) {
            return RecycleStrategy::Flush;
        }
        for (const auto& s : overlaps) {
            const auto& s_params = s->GetSurfaceParams();
            if (s_params.block_depth > 1 || s_params.target == SurfaceTarget::Texture3D) {
                return RecycleStrategy::Flush;
            }
        }
        // Untopological decision
        if (untopological == MatchTopologyResult::CompressUnmatch) {
            return RecycleStrategy::Flush;
        }
        if (untopological == MatchTopologyResult::FullMatch && !params.is_tiled) {
            return RecycleStrategy::Flush;
        }
        return RecycleStrategy::Ignore;
    }

    /**
     * Used to decide what to do with textures we can't resolve in the cache It has 2 implemented
     * strategies: Ignore and Flush.
     *
     * - Ignore: Just unregisters all the overlaps and loads the new texture.
     * - Flush: Flushes all the overlaps into memory and loads the new surface from that data.
     *
     * @param overlaps          The overlapping surfaces registered in the cache.
     * @param params            The parameters for the new surface.
     * @param gpu_addr          The starting address of the new surface.
     * @param untopological     Indicates to the recycler that the texture has no way to match the
     *                          overlaps due to topological reasons.
     **/
    std::pair<TSurface, TView> RecycleSurface(std::vector<TSurface>& overlaps,
                                              const SurfaceParams& params, const GPUVAddr gpu_addr,
                                              const MatchTopologyResult untopological) {
        for (auto& surface : overlaps) {
            Unregister(surface);
        }
        switch (PickStrategy(overlaps, params, gpu_addr, untopological)) {
        case RecycleStrategy::Ignore: {
            return InitializeSurface(gpu_addr, params, Settings::values.use_accurate_gpu_emulation);
        }
        case RecycleStrategy::Flush: {
            std::sort(overlaps.begin(), overlaps.end(),
                      [](const TSurface& a, const TSurface& b) -> bool {
                          return a->GetModificationTick() < b->GetModificationTick();
                      });
            for (auto& surface : overlaps) {
                FlushSurface(surface);
            }
            return InitializeSurface(gpu_addr, params);
        }
        case RecycleStrategy::BufferCopy: {
            auto new_surface = GetUncachedSurface(gpu_addr, params);
            BufferCopy(overlaps[0], new_surface);
            return {new_surface, new_surface->GetMainView()};
        }
        default: {
            UNIMPLEMENTED_MSG("Unimplemented Texture Cache Recycling Strategy!");
            return InitializeSurface(gpu_addr, params);
        }
        }
    }

    /**
     * Takes a single surface and recreates into another that may differ in
     * format, target or width alignment.
     *
     * @param current_surface The registered surface in the cache which we want to convert.
     * @param params          The new surface params which we'll use to recreate the surface.
     * @param is_render       Whether or not the surface is a render target.
     **/
    std::pair<TSurface, TView> RebuildSurface(TSurface current_surface, const SurfaceParams& params,
                                              bool is_render) {
        const auto gpu_addr = current_surface->GetGpuAddr();
        const auto& cr_params = current_surface->GetSurfaceParams();
        TSurface new_surface;
        if (cr_params.pixel_format != params.pixel_format && !is_render &&
            GetSiblingFormat(cr_params.pixel_format) == params.pixel_format) {
            SurfaceParams new_params = params;
            new_params.pixel_format = cr_params.pixel_format;
            new_params.type = cr_params.type;
            new_surface = GetUncachedSurface(gpu_addr, new_params);
        } else {
            new_surface = GetUncachedSurface(gpu_addr, params);
        }
        const auto& final_params = new_surface->GetSurfaceParams();
        if (cr_params.type != final_params.type) {
            if (Settings::values.use_accurate_gpu_emulation) {
                BufferCopy(current_surface, new_surface);
            }
        } else {
            std::vector<CopyParams> bricks = current_surface->BreakDown(final_params);
            for (auto& brick : bricks) {
                ImageCopy(current_surface, new_surface, brick);
            }
        }
        Unregister(current_surface);
        Register(new_surface);
        new_surface->MarkAsModified(current_surface->IsModified(), Tick());
        return {new_surface, new_surface->GetMainView()};
    }

    /**
     * Takes a single surface and checks with the new surface's params if it's an exact
     * match, we return the main view of the registered surface. If its formats don't
     * match, we rebuild the surface. We call this last method a `Mirage`. If formats
     * match but the targets don't, we create an overview View of the registered surface.
     *
     * @param current_surface The registered surface in the cache which we want to convert.
     * @param params          The new surface params which we want to check.
     * @param is_render       Whether or not the surface is a render target.
     **/
    std::pair<TSurface, TView> ManageStructuralMatch(TSurface current_surface,
                                                     const SurfaceParams& params, bool is_render) {
        const bool is_mirage = !current_surface->MatchFormat(params.pixel_format);
        const bool matches_target = current_surface->MatchTarget(params.target);
        const auto match_check = [&]() -> std::pair<TSurface, TView> {
            if (matches_target) {
                return {current_surface, current_surface->GetMainView()};
            }
            return {current_surface, current_surface->EmplaceOverview(params)};
        };
        if (!is_mirage) {
            return match_check();
        }
        if (!is_render && GetSiblingFormat(current_surface->GetFormat()) == params.pixel_format) {
            return match_check();
        }
        return RebuildSurface(current_surface, params, is_render);
    }

    /**
     * Unlike RebuildSurface where we know whether or not registered surfaces match the candidate
     * in some way, we have no guarantees here. We try to see if the overlaps are sublayers/mipmaps
     * of the new surface, if they all match we end up recreating a surface for them,
     * else we return nothing.
     *
     * @param overlaps The overlapping surfaces registered in the cache.
     * @param params   The parameters on the new surface.
     * @param gpu_addr The starting address of the new surface.
     **/
    std::optional<std::pair<TSurface, TView>> TryReconstructSurface(std::vector<TSurface>& overlaps,
                                                                    const SurfaceParams& params,
                                                                    const GPUVAddr gpu_addr) {
        if (params.target == SurfaceTarget::Texture3D) {
            return {};
        }
        bool modified = false;
        TSurface new_surface = GetUncachedSurface(gpu_addr, params);
        u32 passed_tests = 0;
        for (auto& surface : overlaps) {
            const SurfaceParams& src_params = surface->GetSurfaceParams();
            if (src_params.is_layered || src_params.num_levels > 1) {
                // We send this cases to recycle as they are more complex to handle
                return {};
            }
            const std::size_t candidate_size = surface->GetSizeInBytes();
            auto mipmap_layer{new_surface->GetLayerMipmap(surface->GetGpuAddr())};
            if (!mipmap_layer) {
                continue;
            }
            const auto [layer, mipmap] = *mipmap_layer;
            if (new_surface->GetMipmapSize(mipmap) != candidate_size) {
                continue;
            }
            modified |= surface->IsModified();
            // Now we got all the data set up
            const u32 width = SurfaceParams::IntersectWidth(src_params, params, 0, mipmap);
            const u32 height = SurfaceParams::IntersectHeight(src_params, params, 0, mipmap);
            const CopyParams copy_params(0, 0, 0, 0, 0, layer, 0, mipmap, width, height, 1);
            passed_tests++;
            ImageCopy(surface, new_surface, copy_params);
        }
        if (passed_tests == 0) {
            return {};
            // In Accurate GPU all tests should pass, else we recycle
        } else if (Settings::values.use_accurate_gpu_emulation && passed_tests != overlaps.size()) {
            return {};
        }
        for (const auto& surface : overlaps) {
            Unregister(surface);
        }
        new_surface->MarkAsModified(modified, Tick());
        Register(new_surface);
        return {{new_surface, new_surface->GetMainView()}};
    }

    /**
     * Takes care of managing 3D textures and its slices. Does HLE methods for reconstructing the 3D
     * textures within the GPU if possible. Falls back to LLE when it isn't possible to use any of
     * the HLE methods.
     *
     * @param overlaps  The overlapping surfaces registered in the cache.
     * @param params    The parameters on the new surface.
     * @param gpu_addr  The starting address of the new surface.
     * @param cpu_addr  The starting address of the new surface on physical memory.
     */
    std::optional<std::pair<TSurface, TView>> Manage3DSurfaces(std::vector<TSurface>& overlaps,
                                                               const SurfaceParams& params,
                                                               const GPUVAddr gpu_addr,
                                                               const VAddr cpu_addr) {
        if (params.target == SurfaceTarget::Texture3D) {
            bool failed = false;
            if (params.num_levels > 1) {
                // We can't handle mipmaps in 3D textures yet, better fallback to LLE approach
                return std::nullopt;
            }
            TSurface new_surface = GetUncachedSurface(gpu_addr, params);
            bool modified = false;
            for (auto& surface : overlaps) {
                const SurfaceParams& src_params = surface->GetSurfaceParams();
                if (src_params.target != SurfaceTarget::Texture2D) {
                    failed = true;
                    break;
                }
                if (src_params.height != params.height) {
                    failed = true;
                    break;
                }
                if (src_params.block_depth != params.block_depth ||
                    src_params.block_height != params.block_height) {
                    failed = true;
                    break;
                }
                const u32 offset = static_cast<u32>(surface->GetCpuAddr() - cpu_addr);
                const auto offsets = params.GetBlockOffsetXYZ(offset);
                const auto z = std::get<2>(offsets);
                modified |= surface->IsModified();
                const CopyParams copy_params(0, 0, 0, 0, 0, z, 0, 0, params.width, params.height,
                                             1);
                ImageCopy(surface, new_surface, copy_params);
            }
            if (failed) {
                return std::nullopt;
            }
            for (const auto& surface : overlaps) {
                Unregister(surface);
            }
            new_surface->MarkAsModified(modified, Tick());
            Register(new_surface);
            auto view = new_surface->GetMainView();
            return {{std::move(new_surface), view}};
        } else {
            for (const auto& surface : overlaps) {
                if (!surface->MatchTarget(params.target)) {
                    if (overlaps.size() == 1 && surface->GetCpuAddr() == cpu_addr) {
                        if (Settings::values.use_accurate_gpu_emulation) {
                            return std::nullopt;
                        }
                        Unregister(surface);
                        return InitializeSurface(gpu_addr, params);
                    }
                    return std::nullopt;
                }
                if (surface->GetCpuAddr() != cpu_addr) {
                    continue;
                }
                if (surface->MatchesStructure(params) == MatchStructureResult::FullMatch) {
                    return {{surface, surface->GetMainView()}};
                }
            }
            return InitializeSurface(gpu_addr, params);
        }
    }

    /**
     * Gets the starting address and parameters of a candidate surface and tries
     * to find a matching surface within the cache. This is done in 3 big steps:
     *
     * 1. Check the 1st Level Cache in order to find an exact match, if we fail, we move to step 2.
     *
     * 2. Check if there are any overlaps at all, if there are none, we just load the texture from
     *    memory else we move to step 3.
     *
     * 3. Consists of figuring out the relationship between the candidate texture and the
     *    overlaps. We divide the scenarios depending if there's 1 or many overlaps. If
     *    there's many, we just try to reconstruct a new surface out of them based on the
     *    candidate's parameters, if we fail, we recycle. When there's only 1 overlap then we
     *    have to check if the candidate is a view (layer/mipmap) of the overlap or if the
     *    registered surface is a mipmap/layer of the candidate. In this last case we reconstruct
     *    a new surface.
     *
     * @param gpu_addr          The starting address of the candidate surface.
     * @param params            The parameters on the candidate surface.
     * @param is_render         Whether or not the surface is a render target.
     **/
    std::pair<TSurface, TView> GetSurface(const GPUVAddr gpu_addr, const VAddr cpu_addr,
                                          const SurfaceParams& params, bool is_render) {
        // Step 1
        // Check Level 1 Cache for a fast structural match. If candidate surface
        // matches at certain level we are pretty much done.
        if (const auto iter = l1_cache.find(cpu_addr); iter != l1_cache.end()) {
            TSurface& current_surface = iter->second;
            const auto topological_result = current_surface->MatchesTopology(params);
            if (topological_result != MatchTopologyResult::FullMatch) {
                std::vector<TSurface> overlaps{current_surface};
                return RecycleSurface(overlaps, params, gpu_addr, topological_result);
            }

            const auto struct_result = current_surface->MatchesStructure(params);
            if (struct_result != MatchStructureResult::None) {
                const auto& old_params = current_surface->GetSurfaceParams();
                const bool not_3d = params.target != SurfaceTarget::Texture3D &&
                                    old_params.target != SurfaceTarget::Texture3D;
                if (not_3d || current_surface->MatchTarget(params.target)) {
                    if (struct_result == MatchStructureResult::FullMatch) {
                        return ManageStructuralMatch(current_surface, params, is_render);
                    } else {
                        return RebuildSurface(current_surface, params, is_render);
                    }
                }
            }
        }

        // Step 2
        // Obtain all possible overlaps in the memory region
        const std::size_t candidate_size = params.GetGuestSizeInBytes();
        auto overlaps{GetSurfacesInRegion(cpu_addr, candidate_size)};

        // If none are found, we are done. we just load the surface and create it.
        if (overlaps.empty()) {
            return InitializeSurface(gpu_addr, params);
        }

        // Step 3
        // Now we need to figure the relationship between the texture and its overlaps
        // we do a topological test to ensure we can find some relationship. If it fails
        // immediately recycle the texture
        for (const auto& surface : overlaps) {
            const auto topological_result = surface->MatchesTopology(params);
            if (topological_result != MatchTopologyResult::FullMatch) {
                return RecycleSurface(overlaps, params, gpu_addr, topological_result);
            }
        }

        // Check if it's a 3D texture
        if (params.block_depth > 0) {
            auto surface = Manage3DSurfaces(overlaps, params, gpu_addr, cpu_addr);
            if (surface) {
                return *surface;
            }
        }

        // Split cases between 1 overlap or many.
        if (overlaps.size() == 1) {
            TSurface current_surface = overlaps[0];
            // First check if the surface is within the overlap. If not, it means
            // two things either the candidate surface is a supertexture of the overlap
            // or they don't match in any known way.
            if (!current_surface->IsInside(gpu_addr, gpu_addr + candidate_size)) {
                if (current_surface->GetGpuAddr() == gpu_addr) {
                    std::optional<std::pair<TSurface, TView>> view =
                        TryReconstructSurface(overlaps, params, gpu_addr);
                    if (view) {
                        return *view;
                    }
                }
                return RecycleSurface(overlaps, params, gpu_addr, MatchTopologyResult::FullMatch);
            }
            // Now we check if the candidate is a mipmap/layer of the overlap
            std::optional<TView> view =
                current_surface->EmplaceView(params, gpu_addr, candidate_size);
            if (view) {
                const bool is_mirage = !current_surface->MatchFormat(params.pixel_format);
                if (is_mirage) {
                    // On a mirage view, we need to recreate the surface under this new view
                    // and then obtain a view again.
                    SurfaceParams new_params = current_surface->GetSurfaceParams();
                    const u32 wh = SurfaceParams::ConvertWidth(
                        new_params.width, new_params.pixel_format, params.pixel_format);
                    const u32 hh = SurfaceParams::ConvertHeight(
                        new_params.height, new_params.pixel_format, params.pixel_format);
                    new_params.width = wh;
                    new_params.height = hh;
                    new_params.pixel_format = params.pixel_format;
                    std::pair<TSurface, TView> pair =
                        RebuildSurface(current_surface, new_params, is_render);
                    std::optional<TView> mirage_view =
                        pair.first->EmplaceView(params, gpu_addr, candidate_size);
                    if (mirage_view)
                        return {pair.first, *mirage_view};
                    return RecycleSurface(overlaps, params, gpu_addr,
                                          MatchTopologyResult::FullMatch);
                }
                return {current_surface, *view};
            }
        } else {
            // If there are many overlaps, odds are they are subtextures of the candidate
            // surface. We try to construct a new surface based on the candidate parameters,
            // using the overlaps. If a single overlap fails, this will fail.
            std::optional<std::pair<TSurface, TView>> view =
                TryReconstructSurface(overlaps, params, gpu_addr);
            if (view) {
                return *view;
            }
        }
        // We failed all the tests, recycle the overlaps into a new texture.
        return RecycleSurface(overlaps, params, gpu_addr, MatchTopologyResult::FullMatch);
    }

    /**
     * Gets the starting address and parameters of a candidate surface and tries to find a
     * matching surface within the cache that's similar to it. If there are many textures
     * or the texture found if entirely incompatible, it will fail. If no texture is found, the
     * blit will be unsuccessful.
     *
     * @param gpu_addr The starting address of the candidate surface.
     * @param params   The parameters on the candidate surface.
     **/
    Deduction DeduceSurface(const GPUVAddr gpu_addr, const SurfaceParams& params) {
        const std::optional<VAddr> cpu_addr =
            system.GPU().MemoryManager().GpuToCpuAddress(gpu_addr);

        if (!cpu_addr) {
            Deduction result{};
            result.type = DeductionType::DeductionFailed;
            return result;
        }

        if (const auto iter = l1_cache.find(*cpu_addr); iter != l1_cache.end()) {
            TSurface& current_surface = iter->second;
            const auto topological_result = current_surface->MatchesTopology(params);
            if (topological_result != MatchTopologyResult::FullMatch) {
                Deduction result{};
                result.type = DeductionType::DeductionFailed;
                return result;
            }
            const auto struct_result = current_surface->MatchesStructure(params);
            if (struct_result != MatchStructureResult::None &&
                current_surface->MatchTarget(params.target)) {
                Deduction result{};
                result.type = DeductionType::DeductionComplete;
                result.surface = current_surface;
                return result;
            }
        }

        const std::size_t candidate_size = params.GetGuestSizeInBytes();
        auto overlaps{GetSurfacesInRegion(*cpu_addr, candidate_size)};

        if (overlaps.empty()) {
            Deduction result{};
            result.type = DeductionType::DeductionIncomplete;
            return result;
        }

        if (overlaps.size() > 1) {
            Deduction result{};
            result.type = DeductionType::DeductionFailed;
            return result;
        } else {
            Deduction result{};
            result.type = DeductionType::DeductionComplete;
            result.surface = overlaps[0];
            return result;
        }
    }

    /**
     * Gets a null surface based on a target texture.
     * @param target The target of the null surface.
     */
    TView GetNullSurface(SurfaceTarget target) {
        const u32 i_target = static_cast<u32>(target);
        if (const auto it = invalid_cache.find(i_target); it != invalid_cache.end()) {
            return it->second->GetMainView();
        }
        SurfaceParams params{};
        params.target = target;
        params.is_tiled = false;
        params.srgb_conversion = false;
        params.is_layered = false;
        params.block_width = 0;
        params.block_height = 0;
        params.block_depth = 0;
        params.tile_width_spacing = 1;
        params.width = 1;
        params.height = 1;
        params.depth = 1;
        if (target == SurfaceTarget::TextureCubemap || target == SurfaceTarget::TextureCubeArray) {
            params.depth = 6;
        }
        params.pitch = 4;
        params.num_levels = 1;
        params.emulated_levels = 1;
        params.pixel_format = VideoCore::Surface::PixelFormat::R8U;
        params.type = VideoCore::Surface::SurfaceType::ColorTexture;
        auto surface = CreateSurface(0ULL, params);
        invalid_memory.resize(surface->GetHostSizeInBytes(), 0U);
        surface->UploadTexture(invalid_memory);
        surface->MarkAsModified(false, Tick());
        invalid_cache.emplace(i_target, surface);
        return surface->GetMainView();
    }

    /**
     * Gets the a source and destination starting address and parameters,
     * and tries to deduce if they are supposed to be depth textures. If so, their
     * parameters are modified and fixed into so.
     *
     * @param src_params   The parameters of the candidate surface.
     * @param dst_params   The parameters of the destination surface.
     * @param src_gpu_addr The starting address of the candidate surface.
     * @param dst_gpu_addr The starting address of the destination surface.
     **/
    void DeduceBestBlit(SurfaceParams& src_params, SurfaceParams& dst_params,
                        const GPUVAddr src_gpu_addr, const GPUVAddr dst_gpu_addr) {
        auto deduced_src = DeduceSurface(src_gpu_addr, src_params);
        auto deduced_dst = DeduceSurface(src_gpu_addr, src_params);
        if (deduced_src.Failed() || deduced_dst.Failed()) {
            return;
        }

        const bool incomplete_src = deduced_src.Incomplete();
        const bool incomplete_dst = deduced_dst.Incomplete();

        if (incomplete_src && incomplete_dst) {
            return;
        }

        const bool any_incomplete = incomplete_src || incomplete_dst;

        if (!any_incomplete) {
            if (!(deduced_src.IsDepth() && deduced_dst.IsDepth())) {
                return;
            }
        } else {
            if (incomplete_src && !(deduced_dst.IsDepth())) {
                return;
            }

            if (incomplete_dst && !(deduced_src.IsDepth())) {
                return;
            }
        }

        const auto inherit_format = [](SurfaceParams& to, TSurface from) {
            const SurfaceParams& params = from->GetSurfaceParams();
            to.pixel_format = params.pixel_format;
            to.type = params.type;
        };
        // Now we got the cases where one or both is Depth and the other is not known
        if (!incomplete_src) {
            inherit_format(src_params, deduced_src.surface);
        } else {
            inherit_format(src_params, deduced_dst.surface);
        }
        if (!incomplete_dst) {
            inherit_format(dst_params, deduced_dst.surface);
        } else {
            inherit_format(dst_params, deduced_src.surface);
        }
    }

    std::pair<TSurface, TView> InitializeSurface(GPUVAddr gpu_addr, const SurfaceParams& params,
                                                 bool do_load = true) {
        auto new_surface{GetUncachedSurface(gpu_addr, params)};
        Register(new_surface);
        if (do_load) {
            LoadSurface(new_surface);
        }
        return {new_surface, new_surface->GetMainView()};
    }

    void LoadSurface(const TSurface& surface) {
        staging_cache.GetBuffer(0).resize(surface->GetHostSizeInBytes());
        surface->LoadBuffer(system.GPU().MemoryManager(), staging_cache);
        surface->UploadTexture(staging_cache.GetBuffer(0));
        surface->MarkAsModified(false, Tick());
    }

    void FlushSurface(const TSurface& surface) {
        if (!surface->IsModified()) {
            return;
        }
        staging_cache.GetBuffer(0).resize(surface->GetHostSizeInBytes());
        surface->DownloadTexture(staging_cache.GetBuffer(0));
        surface->FlushBuffer(system.GPU().MemoryManager(), staging_cache);
        surface->MarkAsModified(false, Tick());
    }

    void RegisterInnerCache(TSurface& surface) {
        const VAddr cpu_addr = surface->GetCpuAddr();
        VAddr start = cpu_addr >> registry_page_bits;
        const VAddr end = (surface->GetCpuAddrEnd() - 1) >> registry_page_bits;
        l1_cache[cpu_addr] = surface;
        while (start <= end) {
            registry[start].push_back(surface);
            start++;
        }
    }

    void UnregisterInnerCache(TSurface& surface) {
        const VAddr cpu_addr = surface->GetCpuAddr();
        VAddr start = cpu_addr >> registry_page_bits;
        const VAddr end = (surface->GetCpuAddrEnd() - 1) >> registry_page_bits;
        l1_cache.erase(cpu_addr);
        while (start <= end) {
            auto& reg{registry[start]};
            reg.erase(std::find(reg.begin(), reg.end(), surface));
            start++;
        }
    }

    std::vector<TSurface> GetSurfacesInRegion(const VAddr cpu_addr, const std::size_t size) {
        if (size == 0) {
            return {};
        }
        const VAddr cpu_addr_end = cpu_addr + size;
        VAddr start = cpu_addr >> registry_page_bits;
        const VAddr end = (cpu_addr_end - 1) >> registry_page_bits;
        std::vector<TSurface> surfaces;
        while (start <= end) {
            std::vector<TSurface>& list = registry[start];
            for (auto& surface : list) {
                if (!surface->IsPicked() && surface->Overlaps(cpu_addr, cpu_addr_end)) {
                    surface->MarkAsPicked(true);
                    surfaces.push_back(surface);
                }
            }
            start++;
        }
        for (auto& surface : surfaces) {
            surface->MarkAsPicked(false);
        }
        return surfaces;
    }

    void ReserveSurface(const SurfaceParams& params, TSurface surface) {
        surface_reserve[params].push_back(std::move(surface));
    }

    TSurface TryGetReservedSurface(const SurfaceParams& params) {
        auto search{surface_reserve.find(params)};
        if (search == surface_reserve.end()) {
            return {};
        }
        for (auto& surface : search->second) {
            if (!surface->IsRegistered()) {
                return surface;
            }
        }
        return {};
    }

    constexpr PixelFormat GetSiblingFormat(PixelFormat format) const {
        return siblings_table[static_cast<std::size_t>(format)];
    }

    /// Returns true the shader sampler entry is compatible with the TIC texture type.
    static bool IsTypeCompatible(Tegra::Texture::TextureType tic_type,
                                 const VideoCommon::Shader::Sampler& entry) {
        const auto shader_type = entry.GetType();
        switch (tic_type) {
        case Tegra::Texture::TextureType::Texture1D:
        case Tegra::Texture::TextureType::Texture1DArray:
            return shader_type == Tegra::Shader::TextureType::Texture1D;
        case Tegra::Texture::TextureType::Texture1DBuffer:
            // TODO(Rodrigo): Assume as valid for now
            return true;
        case Tegra::Texture::TextureType::Texture2D:
        case Tegra::Texture::TextureType::Texture2DNoMipmap:
            return shader_type == Tegra::Shader::TextureType::Texture2D;
        case Tegra::Texture::TextureType::Texture2DArray:
            return shader_type == Tegra::Shader::TextureType::Texture2D ||
                   shader_type == Tegra::Shader::TextureType::TextureCube;
        case Tegra::Texture::TextureType::Texture3D:
            return shader_type == Tegra::Shader::TextureType::Texture3D;
        case Tegra::Texture::TextureType::TextureCubeArray:
        case Tegra::Texture::TextureType::TextureCubemap:
            if (shader_type == Tegra::Shader::TextureType::TextureCube) {
                return true;
            }
            return shader_type == Tegra::Shader::TextureType::Texture2D && entry.IsArray();
        }
        UNREACHABLE();
        return true;
    }

    struct FramebufferTargetInfo {
        TSurface target;
        TView view;
    };

    VideoCore::RasterizerInterface& rasterizer;

    FormatLookupTable format_lookup_table;

    u64 ticks{};

    // Guards the cache for protection conflicts.
    bool guard_render_targets{};
    bool guard_samplers{};

    // The siblings table is for formats that can inter exchange with one another
    // without causing issues. This is only valid when a conflict occurs on a non
    // rendering use.
    std::array<PixelFormat, static_cast<std::size_t>(PixelFormat::Max)> siblings_table;

    // The internal Cache is different for the Texture Cache. It's based on buckets
    // of 1MB. This fits better for the purpose of this cache as textures are normaly
    // large in size.
    static constexpr u64 registry_page_bits{20};
    static constexpr u64 registry_page_size{1 << registry_page_bits};
    std::unordered_map<VAddr, std::vector<TSurface>> registry;

    static constexpr u32 DEPTH_RT = 8;
    static constexpr u32 NO_RT = 0xFFFFFFFF;

    // The L1 Cache is used for fast texture lookup before checking the overlaps
    // This avoids calculating size and other stuffs.
    std::unordered_map<VAddr, TSurface> l1_cache;

    /// The surface reserve is a "backup" cache, this is where we put unique surfaces that have
    /// previously been used. This is to prevent surfaces from being constantly created and
    /// destroyed when used with different surface parameters.
    std::unordered_map<SurfaceParams, std::vector<TSurface>> surface_reserve;
    std::array<FramebufferTargetInfo, Tegra::Engines::Maxwell3D::Regs::NumRenderTargets>
        render_targets;
    FramebufferTargetInfo depth_buffer;

    std::vector<TSurface> sampled_textures;

    /// This cache stores null surfaces in order to be used as a placeholder
    /// for invalid texture calls.
    std::unordered_map<u32, TSurface> invalid_cache;
    std::vector<u8> invalid_memory;

    StagingCache staging_cache;
    std::recursive_mutex mutex;
};

} // namespace VideoCommon
