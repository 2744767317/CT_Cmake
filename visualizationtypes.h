#pragma once

/*
 * 三正交视图共用的切面方向定义。
 * Axial    : 横断面，沿 Z 轴翻页
 * Coronal  : 冠状面，沿 Y 轴翻页
 * Sagittal : 矢状面，沿 X 轴翻页
 */
enum class SliceOrientation
{
    Axial = 0,
    Coronal,
    Sagittal
};
