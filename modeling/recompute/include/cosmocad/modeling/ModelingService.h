// Реализация IModelingService. Тело детали строится через IGeometryKernel
// и хранится только в памяти: в файл проекта попадают данные построения, а не тело.
#pragma once

#include <map>

#include "cosmocad/modeling/IModelingService.h"

namespace cosmocad::modeling {

class ModelingService final : public IModelingService {
public:
    explicit ModelingService(geometry::IGeometryKernel& kernel) : kernel_(kernel) {}

    core::Result<core::Uuid> addParameter(document::Document& part, const std::string& name,
                                          const std::string& expression) override;
    core::Result<void> setParameter(document::Document& part, const std::string& name,
                                    const std::string& expression) override;
    core::Result<void> removeParameter(document::Document& part, const std::string& name) override;
    core::Result<core::Uuid> addSketch(document::Document& part, const SketchParams& params) override;
    core::Result<void> updateSketch(document::Document& part, const core::Uuid& sketchId,
                                    const std::vector<SketchShape>& shapes) override;
    core::Result<core::Uuid> addExtrude(document::Document& part, const ExtrudeParams& params) override;
    core::Result<core::Uuid> addRevolve(document::Document& part, const RevolveParams& params) override;
    core::Result<core::Uuid> addBlend(document::Document& part, const BlendParams& params) override;
    core::Result<core::Uuid> addMirror(document::Document& part, const MirrorParams& params) override;
    core::Result<core::Uuid> addSweep(document::Document& part, const SweepParams& params) override;
    core::Result<void> saveConfiguration(document::Document& part, const std::string& name,
                                         const std::string& designation = {}) override;
    core::Result<void> applyConfiguration(document::Document& part, const std::string& name) override;
    core::Result<void> removeConfiguration(document::Document& part, const std::string& name) override;
    std::vector<std::string> configurations(const document::Document& part) const override;
    core::Result<void> setFeatureValue(document::Document& part, const core::Uuid& featureId,
                                       const std::string& expression) override;
    core::Result<void> setPattern(document::Document& part, const core::Uuid& featureId,
                                  const Pattern& pattern) override;
    core::Result<void> suppressFeature(document::Document& part, const core::Uuid& featureId,
                                       bool suppressed) override;
    core::Result<void> reorderFeature(document::Document& part, const core::Uuid& featureId,
                                      std::size_t newIndex) override;
    core::Result<void> rollTo(document::Document& part, const core::Uuid& featureId) override;
    core::Result<void> removeFeature(document::Document& part, const core::Uuid& featureId) override;
    core::Result<void> setPartProperties(document::Document& part, const std::string& designation,
                                         const std::string& material, double density) override;
    std::vector<const Feature*> features(const document::Document& part) const override;
    core::Result<std::map<std::string, double>> parameterValues(const document::Document& part) const override;
    core::Result<RecomputeReport> recompute(const document::Document& part,
                                            const std::atomic<bool>* cancel = nullptr) override;
    geometry::ShapePtr body(const document::Document& part) const override;
    std::vector<FeatureStatus> status(const document::Document& part) const override;
    core::Result<geometry::Plane> facePlane(const document::Document& part, const std::string& faceName) const override;
    core::Result<PartMass> mass(const document::Document& part) const override;
    void forget(const core::Uuid& documentId) override { cache_.erase(documentId); }

private:
    struct CacheEntry {
        std::string key;          // отпечаток исходных данных операции и всего, что было до неё
        geometry::ShapePtr body;  // тело после операции
        FeatureStatus status;
    };
    struct DocumentCache {
        std::map<core::Uuid, CacheEntry> entries;
        geometry::ShapePtr body;
        std::vector<FeatureStatus> statuses;
    };

    core::Result<core::Uuid> addFeature(document::Document& part, Feature feature, const char* name,
                                        const char* prefix);

    geometry::IGeometryKernel& kernel_;
    std::map<core::Uuid, DocumentCache> cache_;
};

}  // namespace cosmocad::modeling
