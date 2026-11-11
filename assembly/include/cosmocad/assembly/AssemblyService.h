#pragma once

#include "cosmocad/assembly/IAssemblyService.h"

namespace cosmocad::assembly {

class AssemblyService final : public IAssemblyService {
public:
    AssemblyService(geometry::IGeometryKernel& kernel, IPartSource& parts) : kernel_(kernel), parts_(parts) {}

    core::Result<core::Uuid> addComponent(document::Document& assembly, const core::Uuid& sourceDocumentId,
                                          const Placement& placement, const std::string& name = {}) override;
    core::Result<void> removeComponent(document::Document& assembly, const core::Uuid& componentId) override;
    core::Result<void> setPlacement(document::Document& assembly, const core::Uuid& componentId,
                                    const Placement& placement) override;
    core::Result<void> setFixed(document::Document& assembly, const core::Uuid& componentId, bool fixed) override;
    core::Result<void> setWholeInSection(document::Document& assembly, const core::Uuid& componentId,
                                         bool whole) override;
    core::Result<core::Uuid> addMate(document::Document& assembly, const MateParams& params) override;
    core::Result<void> removeMate(document::Document& assembly, const core::Uuid& mateId) override;
    core::Result<SolveReport> solve(document::Document& assembly, const std::atomic<bool>* cancel = nullptr) override;
    core::Result<std::vector<Freedom>> degreesOfFreedom(const document::Document& assembly) override;
    core::Result<std::vector<PlacedBody>> placedBodies(const document::Document& assembly, bool exploded = false) override;
    core::Result<void> setExplode(document::Document& assembly, const core::Uuid& componentId,
                                  const geometry::Vector3& offset) override;
    core::Result<int> autoExplode(document::Document& assembly, double distance) override;
    core::Result<std::vector<Interference>> checkInterference(const document::Document& assembly,
                                                              const std::atomic<bool>* cancel = nullptr) override;
    core::Result<double> clearance(const document::Document& assembly, const core::Uuid& first,
                                   const core::Uuid& second) override;
    core::Result<std::vector<BomLine>> billOfMaterials(const document::Document& assembly) override;

private:
    geometry::IGeometryKernel& kernel_;
    IPartSource& parts_;
};

}  // namespace cosmocad::assembly
