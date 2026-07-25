#include "UiBridge.h"

#include <cassert>
#include <string>

#include "cpl_json.h"

namespace
{
    CPLJSONObject ParseJson(const std::string& json)
    {
        CPLJSONDocument document;
        const bool loaded = document.LoadMemory(json);
        assert(loaded);
        return document.GetRoot();
    }
} // namespace

int main()
{
    DomDownToolUi::UiBridge bridge;

    const CPLJSONObject sourceResponse = ParseJson(bridge.GetMapSourcesJson());
    assert(sourceResponse.GetBool("ok"));
    assert(sourceResponse.GetArray("groups").Size() == 4);
    const CPLJSONArray sourceGroups = sourceResponse.GetArray("groups");
    assert(sourceGroups[0]
               .GetArray("sources")[0]
               .GetString("previewUrlTemplate")
               .find("uri.amap.com") != std::string::npos);
    const std::string baiduPreviewUrl =
        sourceGroups[3].GetArray("sources")[0].GetString("previewUrlTemplate");
    assert(baiduPreviewUrl.find("location={latitude},{longitude}") != std::string::npos);
    assert(baiduPreviewUrl.find("coord_type=wgs84&") != std::string::npos);
    assert(baiduPreviewUrl.find("src=webapp.domdowntool.preview") != std::string::npos);

    const std::string request = R"([{"project":"范围测试","providerId":"gaode.satellite",)"
                                R"("longitude":116.468067,"latitude":39.912247,"radius":1000,)"
                                R"("zoom":18,"tileSize":256,"outputMode":"merged",)"
                                R"("mergedImageResolution":8192,"fbxTextureSize":4096}])";

    const CPLJSONObject estimateResponse = ParseJson(bridge.EstimatePreviewJson(request));
    assert(estimateResponse.GetBool("ok"));
    assert(estimateResponse.GetLong("tileCount") > 0);
    assert(estimateResponse.GetObj("minimumTile").IsValid());
    assert(estimateResponse.GetObj("maximumTile").IsValid());

    const CPLJSONObject queueResponse = ParseJson(bridge.AddDownloadJobJson(request));
    assert(queueResponse.GetBool("ok"));
    const CPLJSONArray jobs = queueResponse.GetArray("jobs");
    assert(jobs.Size() == 1);
    assert(jobs[0].GetString("providerName") == "高德卫星");
    assert(jobs[0].GetInteger("mergedImageResolution") == 8192);
    assert(jobs[0].GetString("outputDescription") == "大图 / 最大边 8192 px");
    assert(jobs[0].GetString("status") == "等待中");

    const std::string invalidFbxRequest =
        R"([{"project":"范围测试","providerId":"gaode.satellite","longitude":116.468067,)"
        R"("latitude":39.912247,"radius":1000,"zoom":18,"tileSize":256,"outputMode":"fbx",)"
        R"("mergedImageResolution":8192,"fbxTextureSize":3000}])";
    const CPLJSONObject invalidFbxResponse =
        ParseJson(bridge.AddDownloadJobJson(invalidFbxRequest));
    assert(!invalidFbxResponse.GetBool("ok"));

    const std::string zeroRadiusRequest =
        R"([{"project":"范围测试","providerId":"gaode.satellite","longitude":116.468067,)"
        R"("latitude":39.912247,"radius":0,"zoom":18,"tileSize":256,"outputMode":"pyramid"}])";
    const CPLJSONObject zeroRadiusResponse =
        ParseJson(bridge.AddDownloadJobJson(zeroRadiusRequest));
    assert(!zeroRadiusResponse.GetBool("ok"));
    assert(zeroRadiusResponse.GetString("error") == "下载半径必须大于 0。");
    return 0;
}
