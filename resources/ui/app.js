const ui = Object.fromEntries(
    [...document.querySelectorAll("[id]")].map((element) => [element.id, element])
);

const state = {
    groups: [],
    sources: [],
    jobs: [],
    selectedSourceId: "",
    selectedJobId: null,
    mapPreviewUrl: "",
    mapLoadGeneration: 0,
    mapLoadTimerId: null
};

const mapLoadTimeoutMs = 15000;

function normalizeResponse(response) {
    return typeof response === "string" ? JSON.parse(response) : response;
}

async function callNative(name, payload) {
    const bridgeFunction = window[name];
    if (typeof bridgeFunction !== "function") {
        throw new Error("C++ WebView bridge 未连接。");
    }

    const response = payload === undefined
        ? await bridgeFunction()
        : await bridgeFunction(payload);
    return normalizeResponse(response);
}

function getSelectedSource() {
    return state.sources.find((source) => source.id === state.selectedSourceId);
}

function collectParameters() {
    return {
        project: `${getSelectedSource()?.name ?? "地图"}下载任务`,
        providerId: state.selectedSourceId,
        longitude: Number(ui["center-lon"].value),
        latitude: Number(ui["center-lat"].value),
        radius: Number(ui.radius.value),
        zoom: Number(ui.zoom.value),
        tileSize: Number(ui["tile-size"].value),
        outputMode: ui["output-mode"].value,
        mergedImageResolution: Number(ui["merged-resolution"].value),
        fbxTextureSize: Number(ui["fbx-texture-size"].value)
    };
}

function updateOutputSettings() {
    const outputMode = ui["output-mode"].value;
    ui["merged-resolution-field"].hidden = outputMode !== "merged";
    ui["fbx-resolution-field"].hidden = outputMode !== "fbx";
    ui["download-form"].classList.toggle("has-output-resolution", outputMode !== "pyramid");
}

function formatPreviewUrl(template, parameters) {
    if (!template) {
        return "";
    }

    return template
        .replaceAll("{longitude}", encodeURIComponent(parameters.longitude.toFixed(6)))
        .replaceAll("{latitude}", encodeURIComponent(parameters.latitude.toFixed(6)))
        .replaceAll("{zoom}", encodeURIComponent(String(parameters.zoom)));
}

function loadOnlineMap() {
    const source = getSelectedSource();
    const parameters = collectParameters();
    const previewUrl = formatPreviewUrl(source?.previewUrlTemplate, parameters);
    if (!previewUrl) {
        return;
    }

    navigateOnlineMap(previewUrl, false);
}

function navigateOnlineMap(previewUrl, isRetry) {
    if (state.mapLoadTimerId !== null) {
        window.clearTimeout(state.mapLoadTimerId);
    }

    state.mapPreviewUrl = previewUrl;
    const generation = ++state.mapLoadGeneration;
    const navigationUrl = isRetry
        ? `${previewUrl}${previewUrl.includes("?") ? "&" : "?"}domDownToolRetry=${Date.now()}`
        : previewUrl;

    ui["map-loading"].hidden = false;
    ui["map-loading"].classList.remove("failed");
    ui["map-loading"].textContent = isRetry
        ? "正在重试在线地图..."
        : "正在加载在线地图...";
    ui["online-map"].src = navigationUrl;

    state.mapLoadTimerId = window.setTimeout(() => {
        if (generation !== state.mapLoadGeneration) {
            return;
        }

        if (!isRetry) {
            navigateOnlineMap(previewUrl, true);
            return;
        }

        state.mapLoadTimerId = null;
        ui["map-loading"].classList.add("failed");
        ui["map-loading"].textContent = "在线地图加载失败，点击重试";
    }, mapLoadTimeoutMs);
}

function supportsDirectMapLocation(source) {
    return source?.previewUrlTemplate?.startsWith("./map-preview.html") === true;
}

function locateOnlineMap() {
    const source = getSelectedSource();
    if (!supportsDirectMapLocation(source)) {
        loadOnlineMap();
        return;
    }

    const parameters = collectParameters();
    ui["online-map"].contentWindow?.postMessage({
        type: "dom-down-tool-locate",
        longitude: parameters.longitude,
        latitude: parameters.latitude
    }, "*");
}

function clearPreviewRange() {
    ui["range-overlay"].hidden = true;
    ui["tile-range"].textContent = "--";
    ui["tile-count"].textContent = "--";
    ui["current-range-status"].textContent = "范围：未设置";
    showPreviewError("");
}

function setBridgeState(connected, message) {
    ui["bridge-state"].classList.toggle("offline", !connected);
    ui["bridge-state-text"].textContent = message;
}

function setActiveView(viewName) {
    document.querySelectorAll(".tab-button").forEach((button) => {
        button.classList.toggle("active", button.dataset.view === viewName);
    });
    document.querySelectorAll(".view-panel").forEach((panel) => {
        panel.classList.toggle("active", panel.id === `${viewName}-view`);
    });
}

function renderSources() {
    ui["source-list"].replaceChildren();

    for (const group of state.groups) {
        const section = document.createElement("section");
        section.className = "source-group";
        section.innerHTML = `<div class="source-group-header">${group.name}</div>`;

        for (const source of group.sources) {
            const button = document.createElement("button");
            button.type = "button";
            button.className = `source-button${source.id === state.selectedSourceId ? " active" : ""}`;
            button.innerHTML = `
                <span class="source-icon">◆</span>
                <span>${source.name}</span>
                <span class="source-available"></span>`;
            button.addEventListener("click", () => selectSource(source.id));
            section.appendChild(button);
        }

        const cache = document.createElement("div");
        cache.className = "cache-line";
        cache.title = group.cacheRoot;
        cache.textContent = group.cacheRoot;
        section.appendChild(cache);
        ui["source-list"].appendChild(section);
    }

    ui["source-count"].textContent = String(state.sources.length);
}

async function selectSource(sourceId) {
    state.selectedSourceId = sourceId;
    renderSources();

    const source = getSelectedSource();
    ui["map-title"].textContent = source?.name ?? "--";
    ui["selected-provider-label"].textContent = source?.name ?? "--";
    ui["map-coordinate-system"].textContent = source?.coordinate ?? "--";
    ui["source-coordinate"].textContent = source?.coordinate ?? "--";
    ui["offset-strategy"].textContent = source?.offsetStrategy ?? "--";
    ui["current-source-status"].textContent = `地图源：${source?.name ?? "--"}`;
    loadOnlineMap();
    await updatePreviewEstimate();
}

function showPreviewError(message) {
    ui["preview-error"].hidden = !message;
    ui["preview-error"].textContent = message;
}

async function updatePreviewEstimate() {
    if (!state.selectedSourceId) {
        return;
    }

    const parameters = collectParameters();
    ui["center-coordinate"].textContent =
        `${parameters.longitude.toFixed(6)}, ${parameters.latitude.toFixed(6)}`;
    ui["cursor-coordinate"].textContent = ui["center-coordinate"].textContent;

    if (!Number.isFinite(parameters.radius) || parameters.radius <= 0) {
        clearPreviewRange();
        return;
    }

    ui["range-overlay"].hidden = false;

    try {
        const estimate = await callNative("domDownToolEstimatePreview", parameters);
        if (!estimate.ok) {
            throw new Error(estimate.error);
        }

        ui["tile-range"].textContent = estimate.tileRangeText;
        ui["tile-count"].textContent = `${Number(estimate.tileCount).toLocaleString("zh-CN")} 张`;
        ui["cache-path"].textContent = estimate.cacheRoot;
        ui["source-coordinate"].textContent = estimate.coordinate;
        ui["offset-strategy"].textContent = estimate.offsetStrategy;
        ui["current-range-status"].textContent = `范围：${estimate.tileRangeText}`;
        showPreviewError("");
    } catch (error) {
        showPreviewError(error.message);
    }
}

function renderQueue() {
    ui["queue-body"].replaceChildren();
    ui["queue-empty"].hidden = state.jobs.length !== 0;
    ui["queue-summary"].textContent = `${state.jobs.length} 个任务`;

    state.jobs.forEach((job, index) => {
        const row = document.createElement("tr");
        row.classList.toggle("active", job.id === state.selectedJobId);
        row.innerHTML = `
            <td>${index + 1}</td>
            <td>${job.project}</td>
            <td>${job.providerName}</td>
            <td>${job.longitude.toFixed(6)}, ${job.latitude.toFixed(6)}</td>
            <td>${Number(job.radius).toLocaleString("zh-CN")} m</td>
            <td>${job.zoom}</td>
            <td>${job.outputName}</td>
            <td><span class="status-pill">${job.status}</span></td>
            <td><div class="progress-track"><div class="progress-fill" style="width:${job.progress}%"></div></div></td>`;
        row.addEventListener("click", () => {
            state.selectedJobId = job.id;
            renderQueue();
            renderJobDetail();
        });
        ui["queue-body"].appendChild(row);
    });
}

function renderJobDetail() {
    const job = state.jobs.find((candidate) => candidate.id === state.selectedJobId);
    ui["job-status-label"].textContent = job?.status ?? "--";
    ui["job-name"].textContent = job?.project ?? "--";
    ui["job-provider"].textContent = job?.providerName ?? "--";
    ui["job-center"].textContent = job
        ? `${job.longitude.toFixed(6)}, ${job.latitude.toFixed(6)}`
        : "--";
    ui["job-cache"].textContent = job?.cacheRoot ?? "--";
    ui["job-output"].textContent = job?.outputName ?? "--";
    ui["job-output-resolution"].textContent = job?.outputDescription ?? "--";
    ui["job-coordinate"].textContent = job
        ? `${job.coordinate} → WGS84 / ${job.offsetStrategy}`
        : "--";
}

async function addQueueJob(event) {
    event.preventDefault();
    try {
        const snapshot = await callNative("domDownToolAddDownloadJob", collectParameters());
        if (!snapshot.ok) {
            throw new Error(snapshot.error);
        }

        state.jobs = snapshot.jobs;
        state.selectedJobId = state.jobs[0]?.id ?? null;
        renderQueue();
        renderJobDetail();
        showPreviewError("");
        setActiveView("queue");
    } catch (error) {
        showPreviewError(error.message);
    }
}

async function initialize() {
    document.querySelectorAll(".tab-button").forEach((button) => {
        button.addEventListener("click", () => setActiveView(button.dataset.view));
    });
    ui["download-form"].addEventListener("submit", addQueueJob);
    ui["download-form"].querySelectorAll("input, select").forEach((control) => {
        control.addEventListener("change", (event) => {
            updateOutputSettings();
            if (event.target.id === "center-lon" || event.target.id === "center-lat") {
                locateOnlineMap();
            }
            updatePreviewEstimate();
        });
    });
    ui["online-map"].addEventListener("load", () => {
        if (state.mapLoadTimerId !== null) {
            window.clearTimeout(state.mapLoadTimerId);
            state.mapLoadTimerId = null;
        }
        ++state.mapLoadGeneration;
        ui["map-loading"].classList.remove("failed");
        ui["map-loading"].hidden = true;
        if (supportsDirectMapLocation(getSelectedSource())) {
            locateOnlineMap();
        }
    });
    ui["map-loading"].addEventListener("click", () => {
        if (ui["map-loading"].classList.contains("failed") && state.mapPreviewUrl) {
            navigateOnlineMap(state.mapPreviewUrl, true);
        }
    });
    updateOutputSettings();

    try {
        const [sourceSnapshot, queueSnapshot] = await Promise.all([
            callNative("domDownToolGetMapSources"),
            callNative("domDownToolGetQueueSnapshot")
        ]);
        if (!sourceSnapshot.ok || !queueSnapshot.ok) {
            throw new Error(sourceSnapshot.error ?? queueSnapshot.error ?? "本地服务初始化失败。");
        }
        state.groups = sourceSnapshot.groups;
        state.sources = sourceSnapshot.groups.flatMap((group) => group.sources);
        state.jobs = queueSnapshot.jobs;
        state.selectedSourceId = state.sources[0]?.id ?? "";
        state.selectedJobId = state.jobs[0]?.id ?? null;
        setBridgeState(true, "本地服务已连接");
        renderQueue();
        renderJobDetail();
        await selectSource(state.selectedSourceId);
    } catch (error) {
        setBridgeState(false, "本地服务未连接");
        ui["source-list"].innerHTML = `<div class="loading-line">${error.message}</div>`;
        showPreviewError(error.message);
    }
}

initialize();
