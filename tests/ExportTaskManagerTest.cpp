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

#include "cpl_json.h"

#include "preview/ExportTaskManager.h"

namespace
{
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

        CPLJSONDocument persistedDocument;
        if (!postprocess_test::Require(
                persistedDocument.Load(recordsPath.string()),
                "无法读取立即写回的导出任务记录"))
        {
            return false;
        }
        const CPLJSONArray persistedTasks =
            persistedDocument.GetRoot().GetArray("tasks");
        if (!postprocess_test::Require(
                persistedTasks.Size() == 3 &&
                    persistedTasks[0].GetString("state") == "failed" &&
                    persistedTasks[1].GetString("state") == "failed",
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
        invalidConfiguration.mapSourceName = "测试地图";
        invalidConfiguration.radiusMeters = 1000.;
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
    }

    const std::filesystem::path concurrentRecordsPath =
        testRoot / "concurrent-records.json";
    {
        DomDownToolPreview::ExportTaskManager manager(
            [&changeCount] { changeCount.fetch_add(1, std::memory_order_relaxed); },
            concurrentRecordsPath);
        DomDownToolPreview::ExportConfiguration invalidConfiguration;
        invalidConfiguration.mapSourceName = "并发持久化测试";
        invalidConfiguration.radiusMeters = 1000.;

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

        CPLJSONDocument concurrentDocument;
        if (!postprocess_test::Require(
                concurrentDocument.Load(concurrentRecordsPath.string()),
                "并发更新后导出任务记录不是有效 JSON") ||
            !postprocess_test::Require(
                concurrentDocument.GetRoot().GetArray("tasks").Size() ==
                    static_cast<int>(taskIds.size()),
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
