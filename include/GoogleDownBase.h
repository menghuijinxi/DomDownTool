#pragma once
#include "DownToolBase.h"

namespace DownTool
{
	class GoogleDownBase : public DownToolBase
	{
	public:

		virtual void CalculateDownRange() override;
	};
}