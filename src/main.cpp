// DomDownTool.cpp: 定义应用程序的入口点。

#include <iostream>
#include "DownToolBase.h"
#if defined(_WIN32)
#include "windows.h"
#endif
#include "TianDiTuMultidateDown.h"
#include "curl/curl.h"
#include <BaiduDown.h>
#include <GaoDeDown.h>
#include <TencentDown.h>
#include <BingDown.h>
#include <ArcGISDown.h>
#include <GoogleDown.h>

int main()
{
	curl_global_init(CURL_GLOBAL_ALL);

#if defined(_WIN32)
	// Windows系统下，设置控制台输出为UTF-8编码，避免中文乱码
	SetConsoleOutputCP(CP_UTF8);
#endif

	DownTool::GaoDeDown base;
	//base.SetRadius(30000.);
	base.SetZoom(15);
	//base.SetCenterLonLat(DownTool::LonLat(116.927549, 36.712797));
	base.SetRadius(70000.);
	base.SetCenterLonLat(DownTool::LonLat(116.41, 39.92));
	//base.SetPostProcessEnabled(true);
	base.SetMapType(DownTool::GaoDeMapType::Electronic);
	base.SetPostProcessMergedImageEnabled(true);
	base.StartDown();

	return 0;
}
