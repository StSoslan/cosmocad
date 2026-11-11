// Проверка, что объявления интерфейсов, пока не имеющих реализации, собираются
// и согласованы между собой. Реализация появится на этапах 6 и 8 плана.
#include <gtest/gtest.h>

#include "cosmocad/assembly/IAssemblyService.h"
#include "cosmocad/document/IDocumentService.h"
#include "cosmocad/drawing/IDrawingService.h"
#include "cosmocad/geometry/IGeometryKernel.h"
#include "cosmocad/modeling/IModelingService.h"
#include "cosmocad/standards/Engine.h"

using namespace cosmocad;

TEST(Interfaces, EveryInterfaceHasAnApiVersion) {
    EXPECT_GE(geometry::kGeometryKernelApiVersion, 1);
    EXPECT_GE(document::kDocumentServiceApiVersion, 1);
    EXPECT_GE(modeling::kModelingServiceApiVersion, 1);
    EXPECT_GE(standards::kStandardsServiceApiVersion, 1);
    EXPECT_GE(drawing::kDrawingServiceApiVersion, 1);
    EXPECT_GE(assembly::kAssemblyServiceApiVersion, 1);
}

TEST(Interfaces, AreAbstract) {
    EXPECT_TRUE(std::is_abstract_v<geometry::IGeometryKernel>);
    EXPECT_TRUE(std::is_abstract_v<modeling::IModelingService>);
    EXPECT_TRUE(std::is_abstract_v<assembly::IAssemblyService>);
    EXPECT_TRUE(std::is_abstract_v<document::IDocumentService>);
    EXPECT_TRUE(std::is_abstract_v<drawing::IDrawingService>);
    EXPECT_TRUE(std::is_abstract_v<standards::IStandardsService>);
}
