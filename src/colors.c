/*
 * termica - colour schemes. See colors.h.
 */
#include "colors.h"

#include <stdio.h>
#include <string.h>

static const ColorScheme PRESET_DARK = {
    "dark",
    { 216, 222, 233 }, /* foreground */
    {  14,  18,  22 }, /* background (matches original PANEL_BACKGROUND) */
    { 216, 222, 233 }, /* cursor */
    false, { 0, 0, 0 },
    false, { 0, 0, 0 },
    {
        {  29,  32,  33 }, /* 0  black          */
        { 191,  97, 106 }, /* 1  red            */
        { 163, 190, 140 }, /* 2  green          */
        { 235, 203, 139 }, /* 3  yellow         */
        { 129, 161, 193 }, /* 4  blue           */
        { 180, 142, 173 }, /* 5  magenta        */
        { 136, 192, 208 }, /* 6  cyan           */
        { 216, 222, 233 }, /* 7  white          */
        {  76,  86, 106 }, /* 8  bright black   */
        { 208, 135, 112 }, /* 9  bright red     */
        { 179, 206, 156 }, /* 10 bright green   */
        { 242, 214, 155 }, /* 11 bright yellow  */
        { 148, 180, 212 }, /* 12 bright blue    */
        { 199, 161, 192 }, /* 13 bright magenta */
        { 152, 208, 224 }, /* 14 bright cyan    */
        { 236, 239, 244 }  /* 15 bright white   */
    },
    false
};

static const ColorScheme PRESET_LIGHT = {
    "light",
    {  46,  52,  64 }, /* foreground */
    { 245, 246, 248 }, /* background */
    {  46,  52,  64 }, /* cursor */
    true,  { 245, 246, 248 },
    true,  {  76,  86, 106 },
    {
        {  46,  52,  64 }, /* 0  black          */
        { 178,  58,  72 }, /* 1  red            */
        {  68, 128,  68 }, /* 2  green          */
        { 168, 118,  20 }, /* 3  yellow         */
        {  48, 102, 168 }, /* 4  blue           */
        { 138,  78, 148 }, /* 5  magenta        */
        {  38, 126, 142 }, /* 6  cyan           */
        { 216, 222, 233 }, /* 7  white          */
        { 108, 118, 132 }, /* 8  bright black   */
        { 204,  76,  90 }, /* 9  bright red     */
        {  88, 154,  88 }, /* 10 bright green   */
        { 196, 142,  36 }, /* 11 bright yellow  */
        {  68, 124, 192 }, /* 12 bright blue    */
        { 164, 102, 174 }, /* 13 bright magenta */
        {  54, 150, 168 }, /* 14 bright cyan    */
        { 236, 239, 244 }  /* 15 bright white   */
    },
    false
};

bool colors_preset(ColorScheme *scheme, const char *name)
{
    if (!scheme || !name)
        return false;
    if (strcmp(name, "dark") == 0 || strcmp(name, "default") == 0) {
        *scheme = PRESET_DARK;
        return true;
    }
    if (strcmp(name, "light") == 0) {
        *scheme = PRESET_LIGHT;
        return true;
    }
    return false;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool colors_parse_hex(const char *text, ColorRgb *out)
{
    int v[6];
    int i;

    if (!text || !out)
        return false;
    if (text[0] == '#')
        text++;
    if (strlen(text) != 6)
        return false;
    for (i = 0; i < 6; i++) {
        v[i] = hex_val(text[i]);
        if (v[i] < 0)
            return false;
    }
    out->r = (uint8_t)((v[0] << 4) | v[1]);
    out->g = (uint8_t)((v[2] << 4) | v[3]);
    out->b = (uint8_t)((v[4] << 4) | v[5]);
    return true;
}

void colors_format_hex(ColorRgb c, char *out, size_t capacity)
{
    if (!out || capacity < 8)
        return;
    snprintf(out, capacity, "#%02x%02x%02x",
             (unsigned)c.r, (unsigned)c.g, (unsigned)c.b);
}
