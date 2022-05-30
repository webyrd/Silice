/*

    Silice FPGA language and compiler
    Copyright 2019, (C) Sylvain Lefebvre and contributors

    List contributors with: git shortlog -n -s -- <filename>

    GPLv3 license, see LICENSE_GPLv3 in Silice repo root

This program is free software: you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation, either version 3 of the License, or (at your option)
any later version.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program.  If not, see <https://www.gnu.org/licenses/>.

(header_2_G)
*/
// -------------------------------------------------
//                                ... hardcoding ...
// -------------------------------------------------
#include "LuaPreProcessor.h"
#include "Config.h"
#include "Utils.h"
// -------------------------------------------------

#include <iostream>
#include <fstream>
#include <regex>
#include <queue>
#include <filesystem>

#include <LibSL/LibSL.h>

#ifndef EOF
#define EOF -1
#endif
#include <LibSL/CppHelpers/BasicParser.h>

using namespace std;
using namespace antlr4;
using namespace Silice;

// -------------------------------------------------

#include "tga.h"

// -------------------------------------------------

extern "C" {
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
}
#include <luabind/luabind.hpp>
#include <luabind/adopt_policy.hpp>
#include <luabind/operator.hpp>
#include <luabind/exception_handler.hpp>

// -------------------------------------------------

static int numLinesIn(std::string l)
{
  return (int)std::count(l.begin(), l.end(), '\n');
}

// -------------------------------------------------

static void load_config_into_lua(lua_State *L)
{
  luabind::object table = luabind::newtable(L);
  for (auto kv : CONFIG.keyValues()) {
    table[kv.first] = kv.second;
  }
  luabind::globals(L)["config"] = table;
}

// -------------------------------------------------

static void load_config_from_lua(lua_State *L)
{
  luabind::object table = luabind::globals(L)["config"];
  for (luabind::iterator kv(table), end; kv != end; kv++) {
    string key   = luabind::object_cast_nothrow<string>(kv.key(),string(""));
    string value = luabind::object_cast_nothrow<string>(*kv, string(""));
    CONFIG.keyValues()[key] = value;
  }
}

// -------------------------------------------------

LuaPreProcessor::LuaPreProcessor()
{

}

// -------------------------------------------------

LuaPreProcessor::~LuaPreProcessor()
{

}

// -------------------------------------------------

std::string LuaPreProcessor::findFile(std::string path, std::string fname) const
{
  std::string tmp_fname;

  if (LibSL::System::File::exists(fname.c_str())) {
    return fname;
  }
  tmp_fname = path + "/" + extractFileName(fname);
  if (LibSL::System::File::exists(tmp_fname.c_str())) {
    return tmp_fname;
  }
  tmp_fname = path + "/" + fname;
  if (LibSL::System::File::exists(tmp_fname.c_str())) {
    return tmp_fname;
  }
  return fname;
}

// -------------------------------------------------

std::string LuaPreProcessor::findFile(std::string fname) const
{
  for (const auto& path : m_SearchPaths) {
    fname = findFile(path, fname);
  }
  return fname;
}

// -------------------------------------------------

std::map<lua_State*, std::ofstream>    g_LuaOutputs;
std::map<lua_State*, LuaPreProcessor*> g_LuaPreProcessors;

// -------------------------------------------------

static void lua_output(lua_State *L,std::string str,int src_line, int src_file)
{
  auto P = g_LuaPreProcessors.find(L);
  if (P == g_LuaPreProcessors.end()) {
    throw Fatal("[preprocessor] internal error");
  }
  P->second->addingLines(numLinesIn(str), src_line, src_file);
  g_LuaOutputs[L] << str;
}

// -------------------------------------------------

void LuaPreProcessor::addingLines(int num, int src_line,int src_file)
{
  m_FileLineRemapping.push_back(v3i(m_CurOutputLine, src_file, src_line));
  m_CurOutputLine += num;
}

// -------------------------------------------------

static void lua_preproc_error(lua_State *L, std::string str)
{
  lua_error(L);
}

// -------------------------------------------------

static void lua_print(lua_State *L, std::string str)
{
  cerr << "[preprocessor] " << Console::white << str << Console::gray << "\n";
}

// -------------------------------------------------

void lua_dofile(lua_State *L, std::string str)
{
  auto P = g_LuaPreProcessors.find(L);
  if (P == g_LuaPreProcessors.end()) {
    throw Fatal("[preprocessor] internal error");
  }
  LuaPreProcessor *lpp   = P->second;
  std::string      fname = lpp->findFile(str);
  int ret = luaL_dofile(L, fname.c_str());
  if (ret) {
    lua_error(L);
  }
}

// -------------------------------------------------

std::string lua_findfile(lua_State *L, std::string str)
{
  auto P = g_LuaPreProcessors.find(L);
  if (P == g_LuaPreProcessors.end()) {
    throw Fatal("[findfile] internal error");
  }
  LuaPreProcessor *lpp   = P->second;
  std::string      fname = lpp->findFile(str);
  return fname;
}

// -------------------------------------------------

static void lua_write_image_in_table(lua_State* L, std::string str,int component_depth)
{
  auto P = g_LuaPreProcessors.find(L);
  if (P == g_LuaPreProcessors.end()) {
    throw Fatal("[write_image_in_table] internal error");
  }
  if (component_depth < 0 || component_depth > 8) {
    throw Fatal("[write_image_in_table] component depth can only in ]0,8]");
  }
  LuaPreProcessor *lpp   = P->second;
  std::string      fname = lpp->findFile(str);
  t_image_nfo     *nfo   = ReadTGAFile(fname.c_str());
  if (nfo == NULL) {
    throw Fatal("[write_image_in_table] cannot load image file '%s'",fname.c_str());
  }
  int    nc  = nfo->depth/8;
  uchar* ptr = nfo->pixels;
  ForIndex(j, nfo->height) {
    ForIndex(i, nfo->width) {
      uint32_t v = 0;
      ForIndex(c, nc) {
        v = (v << component_depth) | ((*(uint8_t*)(ptr++) >> (8 - component_depth)) & ((1 << component_depth) - 1));
      }
      g_LuaOutputs[L] << std::to_string(v) << ",";
    }
  }
  delete[](nfo->pixels);
  delete[](nfo->colormap);
  delete (nfo);
}

static void lua_write_image_in_table_simple(lua_State* L, std::string str)
{
  lua_write_image_in_table(L, str, 8);
}

// -------------------------------------------------

static void lua_write_palette_in_table(lua_State* L, std::string str, int component_depth)
{
  auto P = g_LuaPreProcessors.find(L);
  if (P == g_LuaPreProcessors.end()) {
    throw Fatal("[write_palette_in_table] internal error");
  }
  if (component_depth < 0 || component_depth > 8) {
    throw Fatal("[write_palette_in_table] component depth can only in ]0,8]");
  }
  LuaPreProcessor *lpp   = P->second;
  std::string      fname = lpp->findFile(str);
  t_image_nfo     *nfo   = ReadTGAFile(fname.c_str());
  if (nfo == NULL) {
    throw Fatal("[write_palette_in_table] cannot load image file '%s'", fname.c_str());
  }
  if (nfo->colormap == NULL) {
    throw Fatal("[write_palette_in_table] image file '%s' has no palette", fname.c_str());
  }
  if (nfo->depth != 8) {
    throw Fatal("[write_palette_in_table] image file '%s' palette is not 8 bits", fname.c_str());
  }
  if (nfo->colormap_chans != 3) {
    throw Fatal("[write_palette_in_table] image file '%s' palette is not RGB", fname.c_str());
  }
  uchar* ptr = nfo->colormap;
  ForIndex(idx, 256) {
      uint32_t v = 0;
      if ((uint)idx < nfo->colormap_size) {
        ForIndex(c, 3) {
          v = (v << component_depth) | ((*(uint8_t *)(ptr++) >> (8 - component_depth)) & ((1 << component_depth) - 1));
        }
      }
      g_LuaOutputs[L] << std::to_string(v) << ",";
  }
  delete[](nfo->pixels);
  delete[](nfo->colormap);
  delete (nfo);
}

// -------------------------------------------------

static void lua_write_palette_in_table_simple(lua_State* L, std::string str)
{
  lua_write_palette_in_table(L, str, 8);
}

// -------------------------------------------------

static luabind::object lua_get_palette_as_table(lua_State* L, std::string str, int component_depth)
{
  auto P = g_LuaPreProcessors.find(L);
  if (P == g_LuaPreProcessors.end()) {
    throw Fatal("[get_palette_as_table] internal error");
  }
  if (component_depth < 0 || component_depth > 8) {
    throw Fatal("[get_palette_as_table] component depth can only be in ]0,8]");
  }
  LuaPreProcessor *lpp   = P->second;
  std::string      fname = lpp->findFile(str);
  t_image_nfo     *nfo   = ReadTGAFile(fname.c_str());
  if (nfo == NULL) {
    throw Fatal("[get_palette_as_table] cannot load image file '%s'", fname.c_str());
  }
  if (nfo->colormap == NULL) {
    throw Fatal("[get_palette_as_table] image file '%s' has no palette", fname.c_str());
  }
  if (nfo->depth != 8) {
    throw Fatal("[get_palette_as_table] image file '%s' palette is not 8 bits", fname.c_str());
  }
  if (nfo->colormap_chans != 3) {
    throw Fatal("[write_palette_in_table] image file '%s' palette is not RGB", fname.c_str());
  }
  luabind::object ltbl = luabind::newtable(L);
  uchar* ptr = nfo->colormap;
  ForIndex(idx, 256) {
    uint32_t v = 0;
    if ((uint)idx < nfo->colormap_size) {
      ForIndex(c, 3) {
        v = (v << component_depth) | ((*(uint8_t *)(ptr++) >> (8 - component_depth)) & ((1 << component_depth) - 1));
      }
    }
    ltbl[1 + idx] = v;
  }
  delete[](nfo->pixels);
  delete[](nfo->colormap);
  delete (nfo);
  return ltbl;
}

// -------------------------------------------------

static luabind::object lua_get_palette_as_table_simple(lua_State *L, std::string str)
{
  return lua_get_palette_as_table(L, str, 8);
}

// -------------------------------------------------

static luabind::object lua_get_image_as_table(lua_State* L, std::string str, int component_depth)
{
  auto P = g_LuaPreProcessors.find(L);
  if (P == g_LuaPreProcessors.end()) {
    throw Fatal("[get_image_as_table] internal error");
  }
  if (component_depth < 0 || component_depth > 8) {
    throw Fatal("[get_image_as_table] component depth can only be in ]0,8]");
  }
  LuaPreProcessor *lpp   = P->second;
  std::string      fname = lpp->findFile(str);
  t_image_nfo     *nfo = ReadTGAFile(fname.c_str());
  if (nfo == NULL) {
    throw Fatal("[get_image_as_table] cannot load image file '%s'", fname.c_str());
  }
  luabind::object rows = luabind::newtable(L);
  int    nc = nfo->depth / 8;
  uchar* ptr = nfo->pixels;
  ForIndex(j, nfo->height) {
    luabind::object cols = luabind::newtable(L);
    ForIndex(i, nfo->width) {
      uint32_t v = 0;
      ForIndex(c, nc) {
        v = (v << component_depth) | ((*(uint8_t*)(ptr++) >> (8 - component_depth)) & ((1 << component_depth) - 1));
      }
      cols[1 + i] = v;
    }
    rows[1 + j] = cols;
  }
  delete[](nfo->pixels);
  delete[](nfo->colormap);
  delete (nfo);
  return rows;
}

// -------------------------------------------------

static luabind::object lua_get_image_as_table_simple(lua_State *L, std::string str)
{
  return lua_get_image_as_table(L, str, 8);
}

// -------------------------------------------------

void lua_save_table_as_image(lua_State *L, luabind::object tbl, std::string fname)
{
  try {
    int i = 0, j = 0;
    int w = 0, h = 0;
    // width / height
    for (luabind::iterator row(tbl), end; row != end; row++) {
      int ncol = 0;
      for (luabind::iterator col(*row), end; col != end; col++) {
        ++ncol;
      }
      if (w != 0 && w != ncol) {
        throw Fatal("[save_table_as_image] row %d does not have the same size as previous", j);
      }
      w = ncol;
      ++h;
      ++j;
    }
    // pixels
    ImageRGB img(w,h);
    j = 0;
    for (luabind::iterator row(tbl), end; row != end; row++) {
      i = 0;
      for (luabind::iterator col(*row), end; col != end; col++) {
        int pix = luabind::object_cast_nothrow<int>(*col,0);
        img.pixel(i, j) = v3b((pix) & 255, (pix >> 8) & 255, (pix >> 16) & 255);
        ++i;
      }
      ++j;
    }
    // save
    saveImage(fname.c_str(), &img);
  } catch (Fatal& f) {
    luaL_error(L,f.message());
  }
}

// -------------------------------------------------

void lua_save_table_as_image_with_palette(lua_State *L,
  luabind::object tbl, luabind::object palette, std::string fname)
{
  try {
    int i = 0, j = 0;
    int w = 0, h = 0;
    // width / height
    for (luabind::iterator row(tbl), end; row != end; row++) {
      int ncol = 0;
      for (luabind::iterator col(*row), end; col != end; col++) {
        ++ncol;
      }
      if (w != 0 && w != ncol) {
        throw Fatal("[save_table_as_image_with_palette] row %d does not have the same size as previous", j);
      }
      w = ncol;
      ++h;
      ++j;
    }
    // pixels
    Array2D<uchar> pixs;
    pixs.allocate(w, h);
    j = 0;
    for (luabind::iterator row(tbl), end; row != end; row++) {
      i = 0;
      for (luabind::iterator col(*row), end; col != end; col++) {
        int pix = luabind::object_cast_nothrow<int>(*col, 0);
        pixs.at(i, j) = pix;
        ++i;
      }
      ++j;
    }
    // palette
    i = 0;
    Array<uint> pal(256);
    for (luabind::iterator p(palette), end; p != end; p++) {
      uint clr = luabind::object_cast_nothrow<uint>(*p, 0);
      if (i == 256) {
        throw Fatal("[save_table_as_image_with_palette] palette has too many entries (expects 256)");
      }
      pal[i++] = (clr >> 16) | (((clr >> 8) & 255) << 8) | ((clr & 255) << 16);
    }
    if (i < 256) {
      throw Fatal("[save_table_as_image_with_palette] palette is missing entries (expects 256)");
    }
    // save
#pragma pack(push, 1)
    /* TGA header */
    struct tga_header_t
    {
      uchar id_length;          /* size of image id */
      uchar colormap_type;      /* 1 if has a colormap */
      uchar image_type;         /* compression type */

      short	cm_first_entry;       /* colormap origin */
      short	cm_length;            /* colormap length */
      uchar cm_depth;            /* colormap depth */

      short	x_origin;             /* bottom left x coord origin */
      short	y_origin;             /* bottom left y coord origin */

      short	width;                /* picture width (in pixels) */
      short	height;               /* picture height (in pixels) */

      uchar pixel_depth;        /* bits per pixel: 8, 16, 24 or 32 */
      uchar image_descriptor;   /* 24 bits = 0x00; 32 bits = 0x80 */
    };
#pragma pack(pop)
    FILE *f = NULL;
    fopen_s(&f, fname.c_str(), "wb");
    if (f == NULL) {
      throw Fatal("sorry, cannot write file '%s'", fname.c_str());
    }
    struct tga_header_t hd;
    hd.id_length = 0;
    hd.colormap_type = 1;
    hd.image_type = 1;
    hd.cm_first_entry = 0;
    hd.cm_length = 256;
    hd.cm_depth = 24;
    hd.x_origin = 0;
    hd.y_origin = 0;
    hd.width = w;
    hd.height = h;
    hd.pixel_depth = 8;
    hd.image_descriptor = (1 << 5);
    fwrite(&hd, sizeof(struct tga_header_t), 1, f);
    ForIndex(p, pal.size()) {
      fwrite(&pal[p], 3, 1, f);
    }
    fwrite(pixs.raw(), w*h, 1, f);
    fclose(f);

  } catch (Fatal& f) {
    luaL_error(L, f.message());
  }
}

// -------------------------------------------------

int lua_lshift(int n,int s)
{
  return n << s;
}

int lua_rshift(int n, int s)
{
  return n >> s;
}

int lua_clog2(int w)
{
  return Utils::justHigherPow2(w);
}

// -------------------------------------------------

static void bindScript(lua_State *L)
{
  luabind::open(L);

  luaL_openlibs(L);

  luabind::module(L)
    [
      luabind::def("print", &lua_print),
      luabind::def("error", &lua_preproc_error),
      luabind::def("output", &lua_output),
      luabind::def("dofile", &lua_dofile),
      luabind::def("findfile", &lua_findfile),
      luabind::def("write_image_in_table", &lua_write_image_in_table),
      luabind::def("write_image_in_table", &lua_write_image_in_table_simple),
      luabind::def("write_palette_in_table", &lua_write_palette_in_table),
      luabind::def("write_palette_in_table", &lua_write_palette_in_table_simple),
      luabind::def("get_image_as_table", &lua_get_image_as_table),
      luabind::def("get_image_as_table", &lua_get_image_as_table_simple),
      luabind::def("get_palette_as_table", &lua_get_palette_as_table),
      luabind::def("get_palette_as_table", &lua_get_palette_as_table_simple),
      luabind::def("save_table_as_image", &lua_save_table_as_image),
      luabind::def("save_table_as_image_with_palette", &lua_save_table_as_image_with_palette),
      luabind::def("clog2",         &lua_clog2),
      luabind::def("lshift",        &lua_lshift),
      luabind::def("rshift",        &lua_rshift)
    ];
}

// -------------------------------------------------

static std::string luaProtectString(std::string str)
{
  str = regex_replace(str, regex("\'"), "\\'");
  return str;
}

// -------------------------------------------------

std::string robustExtractPath(const std::string& path)
{
  // search for last '\\' or '/'
  size_t pos0 = path.rfind("\\");
  size_t pos1 = path.rfind("/");
  size_t pos;
  if (pos0 == string::npos) {
    pos = pos1;
  } else if (pos1 == string::npos) {
    pos = pos0;
  } else {
    pos = max(pos0, pos1);
  }
  if (pos == string::npos) {
    return path;
  }
  string dname = path.substr(0, pos);
  return dname;
}

// -------------------------------------------------

void LuaPreProcessor::enableFilesReport(std::string fname)
{
  m_FilesReportName = fname;
  // create report file, will delete if existing
  std::ofstream freport(m_FilesReportName);
}

// -------------------------------------------------

std::string LuaPreProcessor::assembleSource(
  std::string parent_path,
  std::string src_file,
  std::unordered_set<std::string> alreadyIncluded,
  int& _output_line_count)
{
  cerr << "assembling source " << src_file << '.' << "\n";
  if (!LibSL::System::File::exists(src_file.c_str())) {
    throw Fatal("cannot find source file '%s'", src_file.c_str());
  }
  if (alreadyIncluded.find(src_file) != alreadyIncluded.end()) {
    throw Fatal("source file '%s' already included (cyclic dependency)", src_file.c_str());
  }
  // generate a report with all the loaded files
  if (!m_FilesReportName.empty()) {
    std::ofstream freport(m_FilesReportName, std::ios_base::app);
    freport << std::filesystem::absolute(src_file).string() << '\n';
  }
  // add to already included
  alreadyIncluded.insert(src_file);
  // extract path
  std::string fpath = robustExtractPath(src_file);
  if (fpath == src_file) {
    fpath = ".";
  }
  std::string path = fpath;
  // add to search paths
  m_SearchPaths.push_back(path);
  // add to list of files
  m_Files.emplace_back(std::filesystem::absolute(src_file).string());
  int src_file_id = (int)m_Files.size() - 1;
  // parse, recurse on includes and read code
  LibSL::BasicParser::FileStream fs(src_file.c_str());
  LibSL::BasicParser::Parser<LibSL::BasicParser::FileStream> parser(fs,false);
  std::string code = "";
  int src_line = 0;
  while (!parser.eof()) {
    char next = parser.readChar(false);
    if (next == '$') {
      std::string w = parser.readString("(\n");
      if (w == "$include") {
        parser.readChar(true); // skip (
        auto next = parser.readChar();
        if (next != '"' && next != '\'') {
          // TODO: improve error report
          throw Fatal("parse error in include");
        }
        std::string fname = parser.readString("\"'");
        bool ok = parser.reachChar(')');
        if (!ok) {
          // TODO: improve error report
          throw Fatal("parse error in include");
        }
        // find file
        fname = findFile(path, fname);
        fname = findFile(fname);
        // recurse
        code += assembleSource(path + "/", fname, alreadyIncluded, _output_line_count);
      } else {
        code += " " + w;
      }
    } else if (IS_EOL(next)) {
      // code += " // " + std::to_string(_output_line_count) + " " + src_file + "::" + std::to_string(src_line);
      code += parser.readChar();
      m_SourceFilesLineRemapping.push_back(v3i(_output_line_count, src_file_id, src_line));
      ++ src_line;
      ++ _output_line_count;
    } else {
      std::string w = parser.readString();
      code += " " + w;
    }
  }
  return code;
}

// -------------------------------------------------

class BufferStream : public LibSL::BasicParser::BufferStream
{
public:
  BufferStream(const char *buffer, uint sz) : LibSL::BasicParser::BufferStream(buffer, sz) { }
  int& pos() { return m_Pos; }
};

typedef LibSL::BasicParser::Parser<LibSL::BasicParser::BufferStream> t_Parser;

// -------------------------------------------------

void jumpOverComment(t_Parser& parser)
{
  parser.readChar();
  int next = parser.readChar();
  if (next == '/') {
    // comment, advance until next end of line
    parser.reachChar('\n');
  } else if (next == '*') {
    // block comment, find the end
    while (!parser.eof()) {
      parser.reachChar('*');
      next = parser.readChar();
      if (next == '/') {
        break;
      }
    }
    if (parser.eof()) {
      // TODO: improve error report
      throw Fatal("[parser] Reached end of file while parsing comment block");
    }
  }
}

// -------------------------------------------------

/// \brief This class tracks how the preprocessor code selects
/// parts of the Silice code in the source file. We need to determine this
/// so we only count braces / spaces in one code path. This assumes
/// Lua if/then/else/elseif are only appearing in lua lines ($$),
/// which may not be strictly true but very likely to cover 99% of
/// cases (as using in between $...$ inserts requires advanced trickery.
/// Documentation should specify this.
/// This is also only considering Lua line comments, not the multiline version.
class LuaCodePath
{
private:
  std::vector<bool> m_IfSide;
public:
  LuaCodePath() {}
  void update(t_Parser& parser)
  {
    while (!parser.eof()) {
      int next = parser.readChar(false);
      if (IS_EOL(next)) {
        parser.readChar();
        return; // reached end of lua line
      } else if (next == '"' || next == '\'') {
        parser.readChar();
        // skip over string
        parser.reachChar(next);
      } else {
        std::string w = parser.readString(" \t\r\n\'\"");
        if (w == "--") { // line ends on comment
          return;
        } else if (w == "if" || w == "for" || w == "while" || w == "function") {
          if (m_IfSide.empty()) {
            m_IfSide.push_back(true);
          } else {
            m_IfSide.push_back(m_IfSide.back());
          }
        } else if (w == "end") {
          if (m_IfSide.empty()) {
            // TODO: better message!
            throw Fatal("[parser] Pre-processor directives are unbalanced within the unit, this is not supported.");
          }
          m_IfSide.pop_back();
        } else if (w == "else" || w == "elseif") {
          if (m_IfSide.empty()) {
            // TODO: better message!
            throw Fatal("[parser] Pre-processor directives are unbalanced within the unit, this is not supported.");
          }
          m_IfSide.pop_back();
          m_IfSide.push_back(false);
        }
      }
    }
  }
  bool consider() const { if (m_IfSide.empty()) return true; else return m_IfSide.back(); }
  int  nestLevel() const { return (int)m_IfSide.size(); }
};

// -------------------------------------------------

void processLuaLine(t_Parser& parser, LuaCodePath& lcp)
{
  parser.readChar();
  int next = parser.readChar(false);
  if (next == '$') {
    parser.readChar();
    // -> update the status
    lcp.update(parser);
  }
}

// -------------------------------------------------

void jumpOverNestedBlocks(t_Parser& parser, LuaCodePath& lcp, char c_in, char c_out)
{
  int  inside = 0;
  while (!parser.eof()) {
    int next = parser.readChar(false);
    if (IS_EOL(next)) {
      parser.readChar();
    } else if (next == '/') {
      // might be a comment, jump over it
      jumpOverComment(parser);
    } else if (next == '$') {
      // might be a Lua line
      processLuaLine(parser, lcp);
    } else if (next == c_in) {
      // entering a block
      parser.readChar();
      if (lcp.consider()) {
        ++inside;
      }
    } else if (next == c_out) {
      // exiting a block
      parser.readChar();
      if (lcp.consider()) {
        --inside;
      }
      if (inside == 0) {
        // just exited
        return;
      }
    } else {
      parser.readChar();
    }
  }
  throw Fatal("[parser] Reached end of file while skipping blocks. Are %c %c unbalanced?",
               c_in,c_out);
}

// -------------------------------------------------

void jumpOverUnit(t_Parser& parser, LuaCodePath& lcp)
{
  int nlvl_before = lcp.nestLevel();
  jumpOverNestedBlocks(parser, lcp, '(', ')');
  jumpOverNestedBlocks(parser, lcp, '{', '}');
  int nlvl_after  = lcp.nestLevel();
  if (nlvl_before != nlvl_after) {
    throw Fatal("[parser] Pre-processor directives are spliting the unit, this is not supported:\n"
                "         A unit has to be entirely contained within if-then-else directives");
  }
}

// -------------------------------------------------

void LuaPreProcessor::decomposeSource(const std::string& incode)
{
  BufferStream bs(incode.c_str(),(uint)incode.size());
  t_Parser     parser(bs, false);
  LuaCodePath  lcp;
  std::map<string, v2i> units;

  std::string code = "";
  while (!parser.eof()) {
    int next = parser.readChar(false);
    if (IS_EOL(next)) {
      parser.readChar();
    } else if (next == '/') {
      // might be a comment, jump over it
      jumpOverComment(parser);
    } else if (next == '$') {
      // might be a Lua line
      processLuaLine(parser, lcp);
    } else {
      int before = bs.pos();
      std::string w = parser.readString(" \t\r/*");
      if (w == "unit" || w == "algorithm") {
        std::string name = parser.readString("( \t\r");
        cerr << name << '\n';
        LuaCodePath lcp_unit;
        jumpOverUnit(parser,lcp_unit);
        int after   = bs.pos();
        units[name] = v2i(before, after);
      }
    }
  }
}

// -------------------------------------------------

std::string LuaPreProcessor::prepareCode(std::string header, const std::string& incode)
{
  cerr << "preprocessing " << "\n";

  std::string code  = header;
  int header_offset = numLinesIn(header);

  BufferStream bs(incode.c_str(), (uint)incode.size());
  t_Parser     parser(bs, false);

  std::string current;
  int src_line = 0;
  while (!parser.eof()) {
    int next = parser.readChar(false);
    if (IS_EOL(next)) {
      // -> emit current
      if (!current.empty()) {
        code += "output('";
        code += current;
        code += "\\n'," + std::to_string(header_offset + src_line - 1) + "," + std::to_string(0) + ")\n";
      }
      current = "";
      parser.readChar();
      ++ src_line;
    } else if (next == '\\') {
      // escape sequence
      parser.readChar();
      char ch = parser.readChar();
      if (ch == '"') {
        current += "\\";
      }
      current += ch;
    } else if (next == '$') {
      // Lua line or insertion?
      parser.readChar();
      next = parser.readChar(false);
      if (next == '$') {
        // read line
        parser.readChar();
        std::string lualine = parser.readString("\n");
        code += lualine + "\n";
      } else {
        // read until next $
        std::string luacode = parser.readString("$");
        parser.readChar(); // skip $
        current += "' .. (" + luacode + ") .. '";
        if (incode[bs.pos()] == ' ') {
          current += " ";
        }
      }
    } else {
      current += luaProtectString(parser.readString("\\\n$ "));
      if (incode[bs.pos()] == ' ') {
        current += " ";
      }
    }
  }
  if (!current.empty()) {
    // -> emit current
    code += "output('";
    code += current;
    code += "\\n'," + std::to_string(header_offset + src_line - 1) + "," + std::to_string(0) + ")\n";
  }
  return code;
}

// -------------------------------------------------

// NOTE: use std::filesystem::current_path() in the future ....
#if defined(WIN32) || defined(WIN64)

#include <direct.h>

std::string getCurrentPath()
{
  std::string ret;
  char buf[4096];
  ret = std::string(_getcwd(buf, 4096));
  return ret;
}

std::string fileAbsolutePath(std::string f)
{
  char buf[4096];
  GetFullPathNameA(f.c_str(), 4096, buf, NULL);
  return std::string(buf);
}

#else

#include <unistd.h>
#include <limits.h>

std::string getCurrentPath()
{
  std::string ret;
  char buf[PATH_MAX+1];
  ret = std::string(getcwd(buf, 4096));
  return ret;
}

std::string fileAbsolutePath(std::string f)
{
  char buf[PATH_MAX+1];
  realpath(f.c_str(), buf);
  return std::string(buf);
}

#endif

// -------------------------------------------------

void LuaPreProcessor::run(
  std::string src_file,
  const std::vector<std::string>& defaultLibraries,
  std::string lua_header_code,
  std::string dst_file)
{
  lua_State *L = luaL_newstate();

  g_LuaOutputs.insert(std::make_pair(L, ofstream(dst_file)));
  g_LuaPreProcessors.insert(std::make_pair(L, this));

  // bind intrisics
  bindScript(L);

  // bind definitions
  for (auto dv : m_Definitions) {
    luabind::globals(L)[dv.first] = dv.second;
  }

  // add current directory to search dirs
  m_SearchPaths.push_back(getCurrentPath());
  m_SearchPaths.push_back(extractPath(fileAbsolutePath(src_file)));

  // get code
  std::unordered_set<std::string> inclusions;
  // start with header
  std::string code = "";
  // add default libs to source
  int output_line_count = 0;
  for (auto l : defaultLibraries) {
    std::string libfile = CONFIG.keyValues()["libraries_path"] + "/" + l;
    libfile = findFile(libfile);
    code += assembleSource(CONFIG.keyValues()["libraries_path"], libfile, inclusions, output_line_count);
  }
  // parse main file
  code += assembleSource("", src_file, inclusions, output_line_count);

  {
    ofstream dbg("dbg1.si");
    dbg << code;
  }

  decomposeSource(code);

  code = prepareCode(lua_header_code,code);

  {
    ofstream dbg("dbg2.lua");
    dbg << code;
  }

#if 1
  m_CurOutputLine = 0;

  load_config_into_lua(L);
  int ret = luaL_dostring(L, code.c_str());
  if (ret) {
    char str[4096];
    int errline = -1;
    std::string errmsg = lua_tostring(L, -1);
    snprintf(str, 4049, "[[LUA]exit] %s", errmsg.c_str());
    std::regex  lnum_regex(".*\\:([[:digit:]]+)\\:(.*)");
    std::smatch matches;
    if (std::regex_match(errmsg, matches, lnum_regex)) {
      errline = atoi(matches.str(1).c_str());
      errmsg  = matches.str(2).c_str();
    }
    cerr << "[preprocessor] ";
    cerr << Console::yellow;
    if (errline > -1) {
      cerr << errline << "] " << errmsg << "\n";
    } else {
      cerr << errmsg << "\n";
    }
    cerr << Console::gray;
     throw Fatal("the preprocessor was interrupted");
  }
  load_config_from_lua(L);

  g_LuaOutputs.at(L).close();
  g_LuaOutputs.erase(L);
  g_LuaPreProcessors.erase(L);

  lua_close(L);
#endif
}

// -------------------------------------------------

std::pair<std::string, int> LuaPreProcessor::lineAfterToFileAndLineBefore_search(int line_after, const std::vector<LibSL::Math::v3i>& remap) const
{
  if (line_after < 0) {
    return std::make_pair("", -1);
  }
  // locate line
  int l = 0, r = (int)remap.size()-1;
  while (l < r) {
    int m = (l + r) / 2;
    if (remap[m][0] < line_after) {
      l = m+1;
    } else if (remap[m][0] > line_after) {
      r = m;
    } else {
      return std::make_pair(m_Files[remap[m][1]], remap[m][2]);
    }
  }
  return std::make_pair(m_Files[remap[l][1]], remap[l][2]);
}

// -------------------------------------------------

std::pair<std::string, int> LuaPreProcessor::lineAfterToFileAndLineBefore(int line_after) const
{
  auto prepro = lineAfterToFileAndLineBefore_search(line_after, m_FileLineRemapping);
  // NOTE: prepro.first is not used as this refers to the intermediate assembled file
  return lineAfterToFileAndLineBefore_search(prepro.second, m_SourceFilesLineRemapping);
}

// -------------------------------------------------
