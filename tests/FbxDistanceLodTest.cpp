#include <limits>
#include <string>

#include "nlohmann/json.hpp"

#include "FbxDistanceLod.h"
#include "PostProcessTestSupport.h"
#include "preview/ExportDistanceLodUi.h"

namespace
{
bool RunTest()
{
    using postprocess_test::Require;
    auto settings = DownTool::DefaultFbxDistanceLod();
    if (!Require(settings.Validate().empty(), "default distance rules are invalid"))
    {
        return false;
    }
    for (const auto& [distance, expected] :
         std::vector<std::pair<double, uint32_t>>{
             {0., 4096}, {9999., 4096}, {10000., 2048}, {29999., 2048},
             {30000., 1024}, {50000., 1024}})
    {
        if (!Require(settings.ResolveTextureSize(distance, 4096) == expected,
                "texture threshold selected an incorrect band"))
        {
            return false;
        }
    }
    if (!Require(settings.ResolveMaximumTriangles(0., 0) == 0 &&
            settings.ResolveMaximumTriangles(10000., 0) == 10000 &&
            settings.ResolveMaximumTriangles(30000., 0) == 5000 &&
            settings.ResolveTextureSize(10000., 512) == 512,
            "triangle rules or no-upscaling behavior is incorrect"))
    {
        return false;
    }
    settings.SetTextureRules({{30000., 1024}, {10000., 2048}});
    settings.SetTriangleRules({{20000., 0}, {5000., 128}});
    const auto saved = DownTool::FbxDistanceLodToJson(settings);
    const auto restored = DownTool::FbxDistanceLodFromJson(saved);
    if (!Require(settings.ResolveTextureSize(15000., 4096) == 2048 &&
            settings.ResolveMaximumTriangles(6000., 64) == 128 &&
            settings.ResolveMaximumTriangles(25000., 64) == 0 &&
            restored && *restored == settings,
            "unsorted or independent rules did not survive JSON round trip"))
    {
        return false;
    }
    const auto old = DownTool::FbxDistanceLodFromJson(nlohmann::json::object());
    if (!Require(old && old->GetTextureRules().empty() &&
            old->GetTriangleRules().empty() &&
            old->ResolveTextureSize(50000., 4096) == 4096 &&
            old->ResolveMaximumTriangles(50000., 64) == 64,
            "missing rule arrays changed legacy settings"))
    {
        return false;
    }
    for (const auto& malformed : {
             R"({"textureRules":true})",
             R"({"triangleRules":[{"radiusMeters":10000,"maximumTriangles":-1}]})",
             R"({"textureRules":[{"radiusMeters":"10000","textureSize":1024}]})",
             R"({"textureRules":[{"radiusMeters":10000,"textureSize":1024.5}]})",
             R"({"triangleRules":[{"radiusMeters":0,"maximumTriangles":4294967296}]})"})
    {
        if (!Require(!DownTool::FbxDistanceLodFromJson(nlohmann::json::parse(malformed)),
                "malformed rules were silently accepted"))
        {
            return false;
        }
    }
    for (const auto& rules : std::vector<std::vector<DownTool::FbxDistanceLodRule>>{
             {{-1., 1024}}, {{std::numeric_limits<double>::quiet_NaN(), 1024}},
             {{10000., 1024}, {10000., 2048}}, {{0., 1000}}, {{0., 32768}}})
    {
        settings.SetTextureRules(rules);
        if (!Require(!settings.Validate().empty(), "invalid texture rules passed validation"))
        {
            return false;
        }
    }
    settings.SetTextureRules({});
    settings.SetTriangleRules({{0., 1}});
    if (!Require(!settings.Validate().empty(), "one-triangle budget was accepted"))
    {
        return false;
    }
    DomDownToolPreview::ExportComponent component;
    component.id = "c-base";
    component.kind = DomDownToolPreview::ExportComponentKind::BasePlate;
    component.exportElevation = true;
    component.SetDistanceLod(DownTool::DefaultFbxDistanceLod());
    using DomDownToolPreview::UpdateExportDistanceLod;
    if (!Require(
            UpdateExportDistanceLod(component, "lod-texture-radius", "0", "12000") &&
            UpdateExportDistanceLod(component, "lod-texture-value", "0", "512") &&
            UpdateExportDistanceLod(component, "lod-triangles-value", "0", "0") &&
            UpdateExportDistanceLod(component, "lod-texture-add", "", "") &&
            component.GetDistanceLod().GetTextureRules().size() == 3 &&
            component.GetDistanceLod().GetTextureRules()[0].GetRadiusMeters() == 12000. &&
            component.GetDistanceLod().GetTextureRules()[0].GetValue() == 512 &&
            component.GetDistanceLod().GetTriangleRules()[0].GetValue() == 0 &&
            component.GetDistanceLod().GetTriangleRules()[0].GetRadiusMeters() == 10000.,
            "UI edits mixed texture and triangle arrays or failed to add a rule"))
    {
        return false;
    }
    const auto beforeInvalidEdit = component.GetDistanceLod();
    UpdateExportDistanceLod(component, "lod-texture-radius", "0", "-1");
    UpdateExportDistanceLod(component, "lod-texture-radius", "0", "");
    UpdateExportDistanceLod(component, "lod-triangles-value", "0", "");
    UpdateExportDistanceLod(component, "lod-texture-remove", "", "");
    UpdateExportDistanceLod(component, "lod-triangles-value", "0", "4294967296");
    UpdateExportDistanceLod(component, "lod-texture-remove", "100", "");
    if (!Require(component.GetDistanceLod() == beforeInvalidEdit,
            "invalid UI input changed settings"))
    {
        return false;
    }
    for (int index = 0; index < 3; ++index)
    {
        UpdateExportDistanceLod(component, "lod-texture-remove", "0", "");
    }
    UpdateExportDistanceLod(component, "lod-triangles-remove", "0", "");
    UpdateExportDistanceLod(component, "lod-triangles-remove", "0", "");
    if (!Require(component.GetDistanceLod().GetTextureRules().empty() &&
            component.GetDistanceLod().GetTriangleRules().empty(),
            "UI cannot delete every distance rule"))
    {
        return false;
    }
    std::cout << "FbxDistanceLodTest passed\n";
    return true;
}
}

int main()
{
    return postprocess_test::RunGuarded(RunTest);
}
