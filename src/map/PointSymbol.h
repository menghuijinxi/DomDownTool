#pragma once

#include "AmapVectorPngDownloader.h"

class SkCanvas;

namespace DownTool
{
enum class PointSymbol
{
    Dot, City, District, Address,
    TrafficLight, Parking, Bus, Train, Exit, Taxi, Charging,
    Food, Cafe, Hotel, Shop, Medical, Restroom, Park, Museum, Bank, Home, School
};

PointSymbol classifyPointSymbol(const AmapVectorFeature& feature);
bool hasPointIcon(PointSymbol symbol);
void drawPointIcon(SkCanvas& canvas, PointSymbol symbol, AmapVectorColor color, bool usePalette);
} // namespace DownTool
