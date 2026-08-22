#pragma once

#include <cstring>
#include <string>

namespace M4NativeProviderHomeTemplate {

// System-owned templates for built-in public discovery providers. Protocol data
// remains in the provider/controller; this XML only describes information
// hierarchy.
//
// JJWXC / Fanqie: category tiles + ranked recommend list.
// Legado: bookshelf only — categories are meaningless against a single LAN
// /getBookshelf feed, so tiles are omitted.
inline const char* xmlFor(const std::string& providerId) {
  if (providerId == "legado") {
    return R"XML(<?xml version="1.0"?>
<m4ui version="1" start="home" theme="wap" fullscreen="true">
  <screen id="home" title="@app.name" class="compact">
    <text text="书架" class="hero inset"/>
    <text text="@page.status" class="meta inset"/>
    <list id="books" source="provider.recommend" titleField="title" subtitleField="author" onActivate="provider.openBook" class="ranked inset compact"/>
    <buttons back="返回" primary="详情" left="连接设置" right="刷新" onBack="system.back" onPrimary="provider.openSelected" onLeft="provider.endpoint" onRight="provider.refresh"/>
  </screen>
</m4ui>)XML";
  }
  if (providerId != "jjwxc" && providerId != "fanqie") return nullptr;
  return R"XML(<?xml version="1.0"?>
<m4ui version="1" start="home" theme="wap" fullscreen="true">
  <screen id="home" title="@app.name" class="compact">
    <text text="分类热推" class="hero inset"/>
    <!-- Runtime TTF chrome is quantized to the same fixed system UI faces
         (18/22/26px). No plugin-only uiScale is needed. Tiles render as a flat
         text grid; selection is conveyed by a subtle dithered background. -->
    <tiles id="categories" source="provider.categories" pageSize="8" height="116" onActivate="provider.selectCategory" class="compact"/>
    <text text="@page.status" class="meta inset"/>
    <text text="@page.heading" class="section inset"/>
    <list id="books" source="provider.recommend" titleField="title" subtitleField="author" onActivate="provider.openBook" class="ranked inset compact"/>
    <buttons back="返回" primary="详情" right="刷新" onBack="system.back" onPrimary="provider.openSelected" onRight="provider.refresh"/>
  </screen>
</m4ui>)XML";
}

}  // namespace M4NativeProviderHomeTemplate
