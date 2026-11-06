#include "modules/niri/workspace_strip.hpp"

#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <spdlog/spdlog.h>

#include <algorithm>

#include "modules/niri/workspace_accent.hpp"
#include "modules/niri/workspace_apps.hpp"
#include "modules/niri/workspace_navigation.hpp"
#include "util/icon_loader.hpp"

namespace waybar::modules::niri {

WorkspaceStrip::WorkspaceStrip(const std::string& id, const Bar& bar, const Json::Value& config)
    : AModule(config, "workspace-strip", id, false, false),
      bar_(bar),
      box_(Gtk::ORIENTATION_HORIZONTAL, 2) {
  if (config["icon-size"].isUInt()) {
    icon_size_ = config["icon-size"].asUInt();
  }

  box_.set_name("niri-workspace-strip");
  if (!id.empty()) {
    box_.get_style_context()->add_class(id);
  }
  box_.get_style_context()->add_class(MODULE_CLASS);
  event_box_.add(box_);

  // Active-tile accent is a config value ("accent": "#rrggbb"), not hardcoded
  // CSS: waybar CSS cannot read config, so the module injects a provider
  // derived from the colour. Invalid values fall back to style.css.
  if (config["accent"].isString()) {
    const auto css = activeTileCss(config["accent"].asString());
    if (css.empty()) {
      spdlog::warn("[niri/workspace-strip] invalid accent '{}' — expected #rrggbb",
                   config["accent"].asString());
    } else {
      accent_provider_ = Gtk::CssProvider::create();
      accent_provider_->load_from_data(css);
      box_.get_style_context()->add_provider(accent_provider_,
                                             GTK_STYLE_PROVIDER_PRIORITY_USER);
    }
  }

  // Scroll anywhere over the strip switches workspace, matching the dots.
  event_box_.add_events(Gdk::SCROLL_MASK | Gdk::SMOOTH_SCROLL_MASK);
  box_.add_events(Gdk::SCROLL_MASK | Gdk::SMOOTH_SCROLL_MASK);
  event_box_.signal_scroll_event().connect(
      sigc::mem_fun(*this, &WorkspaceStrip::handleScroll), false);
  box_.signal_scroll_event().connect(sigc::mem_fun(*this, &WorkspaceStrip::handleScroll), false);

  if (!gIPC) {
    gIPC = std::make_unique<IPC>();
  }

  // Every event that can change the visible strip: workspace set, activation,
  // and the window set per workspace.
  for (const auto* ev :
       {"WorkspacesChanged", "WorkspaceActivated", "WorkspaceActiveWindowChanged", "WindowsChanged",
        "WindowOpenedOrChanged", "WindowClosed"}) {
    gIPC->registerForIPC(ev, this);
  }
}

WorkspaceStrip::~WorkspaceStrip() { gIPC->unregisterForIPC(this); }

void WorkspaceStrip::onEvent(const Json::Value& /*ev*/) { dp.emit(); }

bool WorkspaceStrip::handleScroll(GdkEventScroll* event) {
  if (gdk_event_get_pointer_emulated(reinterpret_cast<GdkEvent*>(event))) {
    return false;
  }

  const auto direction = getScrollDir(event);
  if (direction == SCROLL_DIR::NONE) {
    return true;
  }

  int step = 0;
  if (direction == SCROLL_DIR::DOWN || direction == SCROLL_DIR::RIGHT) {
    step = 1;
  } else if (direction == SCROLL_DIR::UP || direction == SCROLL_DIR::LEFT) {
    step = -1;
  } else {
    return true;
  }

  const auto visible = collect();
  std::vector<std::pair<uint64_t, bool>> navigation;
  navigation.reserve(visible.size());
  for (const auto& tile : visible) {
    navigation.emplace_back(tile.id, tile.active);
  }
  const auto target = adjacentVisibleWorkspaceId(navigation, step);
  if (!target) return true;
  const uint64_t target_id = *target;

  try {
    Json::Value request(Json::objectValue);
    auto& action = (request["Action"] = Json::Value(Json::objectValue));
    auto& focusWorkspace = (action["FocusWorkspace"] = Json::Value(Json::objectValue));
    auto& reference = (focusWorkspace["reference"] = Json::Value(Json::objectValue));
    reference["Id"] = target_id;
    IPC::send(request);
  } catch (const std::exception& e) {
    spdlog::error("WorkspaceStrip: scroll action failed: {}", e.what());
    return false;
  }

  return true;
}

void WorkspaceStrip::update() {
  auto tiles = collect();
  auto signature = signatureOf(tiles);
  if (signature != last_signature_) {
    last_signature_ = signature;
    rebuild(tiles);
  }
  AModule::update();
}

std::vector<WorkspaceStrip::TileData> WorkspaceStrip::collect() const {
  std::vector<TileData> tiles;

  auto lock = gIPC->lockData();

  for (const auto& ws : gIPC->workspaces()) {
    // One bar per output: like niri/workspaces, only show this bar's output so
    // another display's active workspace is not highlighted here.
    if (ws["output"].asString() != bar_.output->name) {
      continue;
    }
    TileData tile;
    tile.id = ws["id"].asUInt();
    tile.idx = ws["idx"].asUInt();
    tile.active = ws["is_active"].asBool();

    // Collect the workspace's windows, ordered the way they are laid out so
    // icon order is stable across updates.
    std::vector<Json::Value> windows;
    for (const auto& win : gIPC->windows()) {
      if (win["workspace_id"].asUInt() != tile.id) {
        continue;
      }
      windows.push_back(win);
    }

    std::sort(windows.begin(), windows.end(), [](const Json::Value& a, const Json::Value& b) {
      const auto& la = a["layout"];
      const auto& lb = b["layout"];
      const bool ha = la.isObject() && la["pos_in_scrolling_layout"].isArray();
      const bool hb = lb.isObject() && lb["pos_in_scrolling_layout"].isArray();
      if (!ha && !hb) return a["id"].asUInt64() < b["id"].asUInt64();
      if (!ha) return false;
      if (!hb) return true;
      const int col_a = la["pos_in_scrolling_layout"][0].asInt();
      const int col_b = lb["pos_in_scrolling_layout"][0].asInt();
      if (col_a != col_b) return col_a < col_b;
      return la["pos_in_scrolling_layout"][1].asInt() < lb["pos_in_scrolling_layout"][1].asInt();
    });

    // One icon per window instance: three foot terminals show three icons.
    std::vector<std::string> app_ids;
    app_ids.reserve(windows.size());
    for (const auto& win : windows) {
      app_ids.push_back(win["app_id"].isString() ? win["app_id"].asString() : "");
    }
    tile.app_ids = workspaceAppInstances(app_ids);

    if (!tile.app_ids.empty() || tile.active) {
      tiles.push_back(std::move(tile));
    }
  }

  std::sort(tiles.begin(), tiles.end(),
            [](const TileData& a, const TileData& b) { return a.idx < b.idx; });

  return tiles;
}

std::string WorkspaceStrip::signatureOf(const std::vector<TileData>& tiles) {
  std::string signature;
  for (const auto& tile : tiles) {
    signature += std::to_string(tile.idx);
    signature += tile.active ? "+" : "-";
    for (const auto& app : tile.app_ids) {
      signature += ",";
      signature += app;
    }
    signature += "|";
  }
  return signature;
}

void WorkspaceStrip::rebuild(const std::vector<TileData>& tiles) {
  for (auto* child : box_.get_children()) {
    box_.remove(*child);
  }

  icon_loader_ = std::make_unique<IconLoader>();

  for (const auto& tile : tiles) {
    auto* button = Gtk::make_managed<Gtk::Button>();
    button->set_relief(Gtk::RELIEF_NONE);
    button->get_style_context()->add_class("ws-tile");
    // "inactive" lets style.css target non-active tiles for the glass bevel,
    // separately from the accent gradient on the active tile.
    if (tile.active) {
      button->get_style_context()->add_class("active");
    } else {
      button->get_style_context()->add_class("inactive");
    }
    // A provider attached to the module box does not reach child widgets on
    // GTK3, so attach the accent provider to each tile's own context.
    if (accent_provider_) {
      button->get_style_context()->add_provider(accent_provider_,
                                                GTK_STYLE_PROVIDER_PRIORITY_USER);
    }

    // Buttons swallow scroll before the container sees it — handle it here too.
    button->add_events(Gdk::SCROLL_MASK | Gdk::SMOOTH_SCROLL_MASK);
    button->signal_scroll_event().connect(sigc::mem_fun(*this, &WorkspaceStrip::handleScroll),
                                          false);

    auto* content = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 4);
    auto* label = Gtk::make_managed<Gtk::Label>(std::to_string(tile.idx));
    label->get_style_context()->add_class("ws-index");
    content->pack_start(*label, false, false, 0);

    for (const auto& app : tile.app_ids) {
      auto* image = Gtk::make_managed<Gtk::Image>();
      auto app_info = IconLoader::get_app_info_from_app_id_list(app);
      bool loaded = app_info && icon_loader_->image_load_icon(*image, app_info, icon_size_);
      if (!loaded) {
        auto* fallback = Gtk::make_managed<Gtk::Label>(app.substr(0, 3));
        fallback->get_style_context()->add_class("ws-fallback");
        content->pack_start(*fallback, false, false, 0);
        continue;
      }
      content->pack_start(*image, false, false, 0);
    }

    button->add(*content);

    std::string tooltip;
    for (const auto& app : tile.app_ids) {
      tooltip += (tooltip.empty() ? "" : "\n") + app;
    }
    if (!tooltip.empty()) {
      button->set_tooltip_text(tooltip);
    }

    const auto ws_id = tile.id;
    button->signal_clicked().connect([ws_id] {
      try {
        Json::Value request(Json::objectValue);
        auto& action = (request["Action"] = Json::Value(Json::objectValue));
        auto& focusWorkspace = (action["FocusWorkspace"] = Json::Value(Json::objectValue));
        auto& reference = (focusWorkspace["reference"] = Json::Value(Json::objectValue));
        reference["Id"] = ws_id;
        IPC::send(request);
      } catch (const std::exception& e) {
        spdlog::error("Niri: error focusing workspace: {}", e.what());
      }
    });

    box_.pack_start(*button, false, false, 0);
  }

  box_.show_all();
}

}  // namespace waybar::modules::niri
