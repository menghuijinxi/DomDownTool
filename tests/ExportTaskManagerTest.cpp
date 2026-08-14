#include "PostProcessTestSupport.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "nlohmann/json.hpp"

#include "preview/ExportTaskManager.h"
#include "preview/ExportImageComposer.h"

namespace
{
using Json = nlohmann::json;

Json loadJsonFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return input.is_open() ? Json::parse(input, nullptr, false)
                           : Json::parse(std::string{}, nullptr, false);
}

const DomDownToolPreview::ExportTaskRecord* findRecord(
    const std::vector<DomDownToolPreview::ExportTaskRecord>& records,
    const std::string& id)
{
    const auto record = std::find_if(
        records.begin(), records.end(),
        [&id](const DomDownToolPreview::ExportTaskRecord& item) { return item.id == id; });
    return record == records.end() ? nullptr : &*record;
}

bool waitForFinished(DomDownToolPreview::ExportTaskManager& manager,
                     const std::string& taskId)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline)
    {
        const std::vector<DomDownToolPreview::ExportTaskRecord> records = manager.GetRecords();
        const DomDownToolPreview::ExportTaskRecord* record = findRecord(records, taskId);
        if (record && record->state != DomDownToolPreview::ExportTaskState::Queued &&
            record->state != DomDownToolPreview::ExportTaskState::Running)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

bool runTest()
{
    const std::filesystem::path testRoot =
        std::filesystem::path(DOMDOWNTOOL_SOURCE_DIR) / "tmp" / "export-task-manager-tests";
    const std::filesystem::path recordsPath = testRoot / "records.json";
    std::error_code fileError;
    std::filesystem::remove_all(testRoot, fileError);
    std::filesystem::create_directories(testRoot, fileError);
    if (!postprocess_test::Require(!fileError, "无法准备导出任务测试目录"))
    {
        return false;
    }

    DomDownToolPreview::ExportConfiguration defaultOutputConfiguration;
    DomDownToolPreview::ExportConfiguration selectedOutputConfiguration;
    selectedOutputConfiguration.outputRoot = testRoot / "selected-output";
    if (!postprocess_test::Require(
            DomDownToolPreview::ExportTaskOutputRoot(
                defaultOutputConfiguration, "task-id") ==
                std::filesystem::u8path("cache/导出任务/task-id") &&
                DomDownToolPreview::ExportTaskOutputRoot(
                    selectedOutputConfiguration, "task-id") ==
                selectedOutputConfiguration.outputRoot /
                    std::filesystem::u8path("导出任务/task-id"),
            "默认输出目录和用户选择输出目录没有使用统一的任务结构"))
    {
        return false;
    }

    const std::filesystem::path imageRoot =
        testRoot / std::filesystem::u8path("中文图像目录");
    const std::filesystem::path baseImagePath =
        imageRoot / std::filesystem::u8path("卫星底图.png");
    const std::filesystem::path overlayImagePath =
        imageRoot / std::filesystem::u8path("GIS叠加层.png");
    const std::filesystem::path composedImagePath =
        imageRoot / std::filesystem::u8path("合成结果.png");
    std::filesystem::create_directories(baseImagePath.parent_path(), fileError);
    cv::Mat baseImage(2, 2, CV_8UC4, cv::Scalar(10, 20, 30, 255));
    cv::Mat overlayImage(2, 2, CV_8UC4, cv::Scalar(110, 120, 130, 128));
    baseImage.at<cv::Vec4b>(0, 1) = cv::Vec4b(0, 0, 0, 0);
    const std::filesystem::path asciiBaseImagePath = testRoot / "base.png";
    const std::filesystem::path asciiOverlayImagePath = testRoot / "overlay.png";
    if (!postprocess_test::Require(
            cv::imwrite(
                postprocess_test::PathToUtf8String(asciiBaseImagePath), baseImage) &&
                cv::imwrite(
                    postprocess_test::PathToUtf8String(asciiOverlayImagePath), overlayImage),
            "无法准备 GIS alpha 合成测试图"))
    {
        return false;
    }
    std::filesystem::copy_file(asciiBaseImagePath, baseImagePath, fileError);
    if (!fileError)
    {
        std::filesystem::copy_file(asciiOverlayImagePath, overlayImagePath, fileError);
    }
    std::string composeError;
    if (!postprocess_test::Require(
            !fileError &&
                DomDownToolPreview::ExportImageComposer::CompositeImage(
                    baseImagePath, overlayImagePath, composedImagePath, composeError),
            "无法生成 GIS alpha 合成测试图：" + composeError))
    {
        return false;
    }
    const std::filesystem::path asciiComposedImagePath =
        testRoot / "composed.png";
    std::filesystem::copy_file(
        composedImagePath,
        asciiComposedImagePath,
        std::filesystem::copy_options::overwrite_existing,
        fileError);
    const cv::Mat composedImage = cv::imread(
        postprocess_test::PathToUtf8String(asciiComposedImagePath),
        cv::IMREAD_UNCHANGED);
    const cv::Vec4b composedPixel = composedImage.at<cv::Vec4b>(0, 0);
    const cv::Vec4b transparentBasePixel = composedImage.at<cv::Vec4b>(0, 1);
    if (!postprocess_test::Require(
            composedImage.channels() == 4 && composedPixel[0] == 60 &&
                composedPixel[1] == 70 && composedPixel[2] == 80 &&
                composedPixel[3] == 255 && transparentBasePixel[0] == 110 &&
                transparentBasePixel[1] == 120 && transparentBasePixel[2] == 130 &&
                transparentBasePixel[3] == 128,
            "GIS alpha 合成结果不符合标准 source-over 公式"))
    {
        return false;
    }

    const std::filesystem::path tileRelativePath =
        std::filesystem::path("14") / "1" / "2.png";
    const std::filesystem::path baseTileRoot =
        testRoot / std::filesystem::u8path("瓦片树/卫星XYZ");
    const std::filesystem::path overlayTileRoot =
        testRoot / std::filesystem::u8path("瓦片树/GIS图层");
    const std::filesystem::path outputTileRoot =
        testRoot / std::filesystem::u8path("瓦片树/叠加结果");
    const std::filesystem::path asciiBaseTilePath = testRoot / "base-tile.jpg";
    const cv::Mat baseTile(2, 2, CV_8UC3, cv::Scalar(10, 20, 30));
    if (!postprocess_test::Require(
            cv::imwrite(
                postprocess_test::PathToUtf8String(asciiBaseTilePath), baseTile),
            "无法准备 JPEG 卫星瓦片"))
    {
        return false;
    }
    std::filesystem::create_directories(
        (baseTileRoot / tileRelativePath).parent_path(), fileError);
    std::filesystem::create_directories(
        (overlayTileRoot / tileRelativePath).parent_path(), fileError);
    std::filesystem::copy_file(
        asciiBaseTilePath, baseTileRoot / tileRelativePath, fileError);
    if (!fileError)
    {
        std::filesystem::copy_file(
            asciiOverlayImagePath, overlayTileRoot / tileRelativePath, fileError);
    }
    composeError.clear();
    if (!postprocess_test::Require(
            !fileError && DomDownToolPreview::ExportImageComposer::CopyDirectory(
                baseTileRoot, outputTileRoot, composeError) &&
                DomDownToolPreview::ExportImageComposer::CompositeXyzTileTree(
                    outputTileRoot, overlayTileRoot, outputTileRoot, composeError),
            "中文 XYZ 目录无法叠加 GIS 图层：" + composeError))
    {
        return false;
    }
    const std::filesystem::path asciiComposedTilePath =
        testRoot / "composed-tile.png";
    std::filesystem::copy_file(
        outputTileRoot / tileRelativePath,
        asciiComposedTilePath,
        std::filesystem::copy_options::overwrite_existing,
        fileError);
    const cv::Mat composedTile = cv::imread(
        postprocess_test::PathToUtf8String(asciiComposedTilePath),
        cv::IMREAD_UNCHANGED);
    const cv::Vec3b composedTilePixel = composedTile.at<cv::Vec3b>(0, 0);
    if (!postprocess_test::Require(
            !fileError &&
                postprocess_test::RequireJpegEncoding(
                    outputTileRoot / tileRelativePath) &&
                composedTile.channels() == 3 &&
                composedTilePixel[0] == 60 && composedTilePixel[1] == 70 &&
                composedTilePixel[2] == 80,
            "XYZ 卫星瓦片与 GIS 图层没有正确合成"))
    {
        return false;
    }

    const std::filesystem::path fbxTextureRelativePath = "0_0.png";
    const std::filesystem::path baseFbxRoot =
        testRoot / std::filesystem::u8path("FBX贴图/卫星图");
    const std::filesystem::path overlayFbxRoot =
        testRoot / std::filesystem::u8path("FBX贴图/GIS图层");
    std::filesystem::create_directories(baseFbxRoot, fileError);
    std::filesystem::create_directories(overlayFbxRoot, fileError);
    std::filesystem::copy_file(
        asciiBaseImagePath, baseFbxRoot / fbxTextureRelativePath, fileError);
    if (!fileError)
    {
        std::filesystem::copy_file(
            asciiOverlayImagePath,
            overlayFbxRoot / fbxTextureRelativePath,
            fileError);
    }
    composeError.clear();
    if (!postprocess_test::Require(
            !fileError && DomDownToolPreview::ExportImageComposer::CompositeTileTree(
                baseFbxRoot, overlayFbxRoot, baseFbxRoot, composeError),
            "中文 FBX 贴图目录无法叠加 GIS 图层：" + composeError))
    {
        return false;
    }
    const std::filesystem::path asciiComposedFbxTexturePath =
        testRoot / "composed-fbx-texture.png";
    std::filesystem::copy_file(
        baseFbxRoot / fbxTextureRelativePath,
        asciiComposedFbxTexturePath,
        std::filesystem::copy_options::overwrite_existing,
        fileError);
    const cv::Mat composedFbxTexture = cv::imread(
        postprocess_test::PathToUtf8String(asciiComposedFbxTexturePath),
        cv::IMREAD_UNCHANGED);
    const cv::Vec4b composedFbxPixel = composedFbxTexture.at<cv::Vec4b>(0, 0);
    if (!postprocess_test::Require(
            !fileError &&
                postprocess_test::RequirePngEncoding(
                    baseFbxRoot / fbxTextureRelativePath) &&
                composedFbxTexture.channels() == 4 && composedFbxPixel[0] == 60 &&
                composedFbxPixel[1] == 70 && composedFbxPixel[2] == 80 &&
                composedFbxPixel[3] == 255,
            "FBX 卫星贴图与 GIS 图层没有正确合成"))
    {
        return false;
    }

    std::ofstream output(recordsPath, std::ios::binary | std::ios::trunc);
    output << R"json({
  "version": 1,
  "tasks": [
    {"id":"queued","title":"排队任务","detail":"GIS XYZ","outputPath":"gis",
     "createdAt":"2026-08-10 10:00:00","finishedAt":"","state":"queued",
     "stage":"等待执行","progress":0,"completed":0,"total":10,"error":""},
    {"id":"running","title":"运行任务","detail":"卫星大图","outputPath":"satellite",
     "createdAt":"2026-08-10 10:01:00","finishedAt":"","state":"running",
     "stage":"下载瓦片","progress":0.5,"completed":5,"total":10,"error":""},
    {"id":"done","title":"完成任务","detail":"GIS 大图","outputPath":"gis",
     "createdAt":"2026-08-10 09:00:00","finishedAt":"2026-08-10 09:01:00",
     "state":"succeeded","stage":"已完成","progress":1,"completed":1,"total":1,
     "error":""}
  ]
})json";
    output.close();

    std::atomic_uint32_t changeCount = 0;
    {
        DomDownToolPreview::ExportTaskManager manager(
            [&changeCount] { changeCount.fetch_add(1, std::memory_order_relaxed); },
            recordsPath);
        const std::vector<DomDownToolPreview::ExportTaskRecord> records = manager.GetRecords();
        const DomDownToolPreview::ExportTaskRecord* queued = findRecord(records, "queued");
        const DomDownToolPreview::ExportTaskRecord* running = findRecord(records, "running");
        const DomDownToolPreview::ExportTaskRecord* done = findRecord(records, "done");
        if (!postprocess_test::Require(
                queued && queued->state == DomDownToolPreview::ExportTaskState::Failed &&
                    queued->stage == "已中断" && !queued->finishedAt.empty(),
                "重启后排队任务没有标记为已中断") ||
            !postprocess_test::Require(
                running && running->state == DomDownToolPreview::ExportTaskState::Failed &&
                    running->stage == "已中断" && !running->finishedAt.empty(),
                "重启后运行任务没有标记为已中断") ||
            !postprocess_test::Require(
                done && done->state == DomDownToolPreview::ExportTaskState::Succeeded,
                "已完成任务的历史状态被修改"))
        {
            return false;
        }

        const Json persistedRoot = loadJsonFile(recordsPath);
        if (!postprocess_test::Require(
                !persistedRoot.is_discarded() && persistedRoot.is_object(),
                "无法读取立即写回的导出任务记录"))
        {
            return false;
        }
        const auto persistedTasks = persistedRoot.find("tasks");
        if (!postprocess_test::Require(
                persistedTasks != persistedRoot.end() && persistedTasks->is_array() &&
                    persistedTasks->size() == 3 &&
                    (*persistedTasks)[0].value("state", "") == "failed" &&
                    (*persistedTasks)[1].value("state", "") == "failed",
                "中断状态没有立即持久化"))
        {
            return false;
        }

        manager.ClearFinished();
        if (!postprocess_test::Require(manager.GetRecords().empty(),
                                       "清除已结束记录没有移除历史任务"))
        {
            return false;
        }

        DomDownToolPreview::ExportConfiguration invalidConfiguration;
        invalidConfiguration.radiusMeters = 1000.;
        invalidConfiguration.outputRoot = testRoot / "invalid-output";
        invalidConfiguration.satelliteSources.push_back({
            "gaode.satellite", "高德卫星", testRoot / "satellite-cache"});
        const std::string invalidTaskId = manager.Enqueue(invalidConfiguration);
        if (!postprocess_test::Require(waitForFinished(manager, invalidTaskId),
                                       "无效导出任务没有结束"))
        {
            return false;
        }
        const std::vector<DomDownToolPreview::ExportTaskRecord> invalidRecords =
            manager.GetRecords();
        const DomDownToolPreview::ExportTaskRecord* invalidRecord =
            findRecord(invalidRecords, invalidTaskId);
        if (!postprocess_test::Require(
                invalidRecord &&
                    invalidRecord->state == DomDownToolPreview::ExportTaskState::Failed &&
                    invalidRecord->error == "没有选择任何导出内容。",
                "工作线程没有记录导出失败原因"))
        {
            return false;
        }
        const std::filesystem::path conversionRecord =
            invalidConfiguration.outputRoot / std::filesystem::u8path("导出任务") /
            std::filesystem::u8path(invalidTaskId) /
            std::filesystem::u8path("转换记录-" + invalidTaskId + ".txt");
        if (!postprocess_test::Require(
                postprocess_test::ContainsText(conversionRecord, "状态: 失败") &&
                    postprocess_test::ContainsText(
                        conversionRecord, "结果: 没有选择任何导出内容。") &&
                    postprocess_test::ContainsText(conversionRecord, "半径 (米): 1000") &&
                    postprocess_test::ContainsText(conversionRecord, "XYZ 目录:") &&
                    postprocess_test::ContainsText(conversionRecord, "/XYZ") &&
                    postprocess_test::ContainsText(conversionRecord, "FBX 目录:") &&
                    postprocess_test::ContainsText(conversionRecord, "/FBX"),
                "输出目录没有保存可复用的任务转换记录"))
        {
            return false;
        }

        if (!postprocess_test::Require(
                manager.RemoveFinished(invalidTaskId) &&
                    findRecord(manager.GetRecords(), invalidTaskId) == nullptr &&
                    std::filesystem::exists(conversionRecord),
                "移除单个任务记录时不应删除导出文件"))
        {
            return false;
        }

        const std::string deleteTaskId = manager.Enqueue(invalidConfiguration);
        if (!postprocess_test::Require(waitForFinished(manager, deleteTaskId),
                                       "永久删除测试任务没有结束"))
        {
            return false;
        }
        const std::filesystem::path deleteTaskOutput =
            DomDownToolPreview::ExportTaskOutputRoot(invalidConfiguration, deleteTaskId);
        std::string deleteError;
        if (!postprocess_test::Require(
                std::filesystem::exists(deleteTaskOutput) &&
                    manager.DeleteFinishedOutput(deleteTaskId, deleteError) &&
                    !std::filesystem::exists(deleteTaskOutput) &&
                    findRecord(manager.GetRecords(), deleteTaskId) == nullptr,
                "永久删除没有同时删除任务目录和任务记录：" + deleteError))
        {
            return false;
        }
    }

    const std::filesystem::path protectedOutput = testRoot / "protected-output";
    std::filesystem::create_directories(protectedOutput, fileError);
    std::ofstream(protectedOutput / "keep.txt") << "keep";
    const std::filesystem::path unsafeRecordsPath = testRoot / "unsafe-records.json";
    std::ofstream unsafeRecords(unsafeRecordsPath, std::ios::binary | std::ios::trunc);
    unsafeRecords << "{\"version\":1,\"tasks\":[{\"id\":\"unsafe\","
                     "\"title\":\"异常路径\",\"detail\":\"test\",\"outputPath\":\""
                  << protectedOutput.generic_string()
                  << "\",\"createdAt\":\"2026-08-10 10:00:00\",\"finishedAt\":"
                     "\"2026-08-10 10:01:00\",\"state\":\"succeeded\","
                     "\"stage\":\"已完成\",\"progress\":1,\"completed\":1,"
                     "\"total\":1,\"error\":\"\"}]}";
    unsafeRecords.close();
    {
        DomDownToolPreview::ExportTaskManager manager({}, unsafeRecordsPath);
        std::string deleteError;
        if (!postprocess_test::Require(
                !manager.DeleteFinishedOutput("unsafe", deleteError) &&
                    std::filesystem::exists(protectedOutput / "keep.txt") &&
                    findRecord(manager.GetRecords(), "unsafe") != nullptr &&
                    !deleteError.empty(),
                "永久删除没有拒绝异常任务路径"))
        {
            return false;
        }
    }

    const std::filesystem::path concurrentRecordsPath =
        testRoot / "concurrent-records.json";
    {
        DomDownToolPreview::ExportTaskManager manager(
            [&changeCount] { changeCount.fetch_add(1, std::memory_order_relaxed); },
            concurrentRecordsPath);
        DomDownToolPreview::ExportConfiguration invalidConfiguration;
        invalidConfiguration.radiusMeters = 1000.;
        invalidConfiguration.outputRoot = testRoot / "concurrent-output";

        std::mutex taskIdsMutex;
        std::vector<std::string> taskIds;
        std::vector<std::thread> producers;
        for (size_t producerIndex = 0; producerIndex < 4; ++producerIndex)
        {
            producers.emplace_back(
                [&manager, &invalidConfiguration, &taskIdsMutex, &taskIds]
                {
                    for (size_t taskIndex = 0; taskIndex < 6; ++taskIndex)
                    {
                        const std::string taskId = manager.Enqueue(invalidConfiguration);
                        const std::lock_guard lock(taskIdsMutex);
                        taskIds.push_back(taskId);
                    }
                });
        }
        for (std::thread& producer : producers)
        {
            producer.join();
        }
        for (const std::string& taskId : taskIds)
        {
            if (!postprocess_test::Require(
                    waitForFinished(manager, taskId),
                    "并发写入时导出任务没有结束"))
            {
                return false;
            }
        }

        const Json concurrentRoot = loadJsonFile(concurrentRecordsPath);
        const auto concurrentTasks = concurrentRoot.is_object()
            ? concurrentRoot.find("tasks")
            : concurrentRoot.end();
        if (!postprocess_test::Require(
                !concurrentRoot.is_discarded() && concurrentRoot.is_object(),
                "并发更新后导出任务记录不是有效 JSON") ||
            !postprocess_test::Require(
                concurrentTasks != concurrentRoot.end() && concurrentTasks->is_array() &&
                    concurrentTasks->size() == taskIds.size(),
                "并发更新后导出任务记录数量不完整"))
        {
            return false;
        }
    }

    return postprocess_test::Require(
        changeCount.load(std::memory_order_relaxed) >= 3,
        "导出任务状态变化没有通知界面");
}
} // namespace

int main()
{
    return postprocess_test::RunGuarded(runTest);
}
