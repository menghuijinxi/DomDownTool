// DomDownTool.cpp: 定义应用程序的入口点。

#include <iostream>
#include "DownToolBase.h"
#include "TianDiTuDown.h"
#if WIN32
#include "windows.h"
#endif
#include "curl/curl.h"

using namespace std;

int main()
{
	curl_global_init(CURL_GLOBAL_ALL);

#if WIN32
	// Windows系统下，设置控制台输出为UTF-8编码，避免中文乱码
	SetConsoleOutputCP(CP_UTF8);
#endif

	DownTool::TianDiTuDown base;
	base.SetRadius(1000.);
	base.SetCenterLonLat(DownTool::LonLat(116.468067, 39.912247));
	base.StartDown();

	std::printf("下载完毕");
	return 0;
}
