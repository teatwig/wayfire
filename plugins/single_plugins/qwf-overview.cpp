#include "plugins/ipc/ipc-activator.hpp"
#include "wayfire/geometry.hpp"

#include <tuple>
#include <wayfire/plugins/common/input-grab.hpp>
#include <wayfire/plugins/common/util.hpp>

#include <wayfire/core.hpp>
#include <wayfire/debug.hpp>
#include <wayfire/object.hpp>
#include <wayfire/opengl.hpp>
#include <wayfire/output.hpp>
#include <wayfire/per-output-plugin.hpp>
#include <wayfire/render-manager.hpp>
#include <wayfire/scene-input.hpp>
#include <wayfire/scene-operations.hpp>
#include <wayfire/scene-render.hpp>
#include <wayfire/scene.hpp>
#include <wayfire/seat.hpp>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/util/duration.hpp>
#include <wayfire/view-helpers.hpp>
#include <wayfire/view-transform.hpp>
#include <wayfire/view.hpp>
#include <wayfire/window-manager.hpp>
#include <wayfire/workspace-set.hpp>

#include <linux/input-event-codes.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <numeric>
#include <vector>

constexpr const char *window_transformer_name = "qwf-overview";
constexpr const char *background_transformer_name = "qwf-overview";
constexpr float background_dim_factor = 0.6;
constexpr float col_spacing = 0.0;
constexpr float row_spacing = 0.0;
constexpr float window_preview_max_scale = 0.95;

using namespace wf::animation;
class QWFOverviewPaintAttribs {
public:
  QWFOverviewPaintAttribs(const duration_t &duration) : scale_x(duration, 1, 1), scale_y(duration, 1, 1),
                                                        off_x(duration, 0, 0), off_y(duration, 0, 0) {}

  timed_transition_t scale_x, scale_y;
  timed_transition_t off_x, off_y;
};

struct QWFOverviewView {
  wayfire_toplevel_view view;
  QWFOverviewPaintAttribs attribs;

  QWFOverviewView(duration_t &duration) : attribs(duration) {}

  /* Make animation start values the current progress of duration */
  void refresh_start() {
    for_each([](timed_transition_t &t) { t.restart_same_end(); });
  }

  void to_end() {
    for_each([](timed_transition_t &t) { t.set(t.end, t.end); });
  }

private:
  void for_each(std::function<void(timed_transition_t &t)> call) {
    call(attribs.off_x);
    call(attribs.off_y);

    call(attribs.scale_x);
    call(attribs.scale_y);
  }
};

struct QWFOverviewRow {
  float full_height, full_width;
  float x, y, width, height;
  float extra_scale;
  std::vector<int> view_idxs;
};

class QWFOverview : public wf::per_output_plugin_instance_t, public wf::pointer_interaction_t {
  wf::ipc_activator_t toggle_overview{"qwf-overview/toggle"};
  wf::option_wrapper_t<wf::animation_description_t> speed{"qwf-overview/speed"};

  duration_t duration{speed};
  duration_t background_dim_duration{speed};
  timed_transition_t background_dim{background_dim_duration};

  std::unique_ptr<wf::input_grab_t> input_grab;

  /* If a view comes before another in this list, it is on top of it */
  std::vector<QWFOverviewView> views;

  bool active = false;

  class overview_render_node_t : public wf::scene::node_t {
    class overview_render_instance_t : public wf::scene::render_instance_t {
      std::shared_ptr<overview_render_node_t> self;
      wf::scene::damage_callback push_damage;
      wf::signal::connection_t<wf::scene::node_damage_signal> on_overview_damage =
          [=](wf::scene::node_damage_signal *ev) {
            push_damage(ev->region);
          };

    public:
      overview_render_instance_t(overview_render_node_t *self, wf::scene::damage_callback push_damage) {
        this->self = std::dynamic_pointer_cast<overview_render_node_t>(self->shared_from_this());
        this->push_damage = push_damage;
        self->connect(&on_overview_damage);
      }

      void schedule_instructions(
          std::vector<wf::scene::render_instruction_t> &instructions,
          const wf::render_target_t &target, wf::region_t &damage) override {
        instructions.push_back(wf::scene::render_instruction_t{
            .instance = this,
            .target = target,
            .damage = damage & self->get_bounding_box(),
        });

        // Don't render anything below
        auto bbox = self->get_bounding_box();
        damage ^= bbox;
      }

      void render(const wf::render_target_t &target, const wf::region_t &region, const std::any &tag) override {
        self->overview->render(target.translated(-wf::origin(self->get_bounding_box())));
      }
    };

  public:
    overview_render_node_t(QWFOverview *overview) : node_t(false) {
      this->overview = overview;
    }

    virtual void gen_render_instances(
        std::vector<wf::scene::render_instance_uptr> &instances,
        wf::scene::damage_callback push_damage, wf::output_t *shown_on) {
      if (shown_on != this->overview->output) {
        return;
      }

      instances.push_back(std::make_unique<overview_render_instance_t>(this, push_damage));
    }

    wf::geometry_t get_bounding_box() {
      return overview->output->get_layout_geometry();
    }

  private:
    QWFOverview *overview;
  };

  std::shared_ptr<overview_render_node_t> render_node;
  wf::plugin_activation_data_t grab_interface = {
      .name = "qwf-overview",
      .capabilities = wf::CAPABILITY_MANAGE_COMPOSITOR,
  };

public:
  void init() override {
    // TODO remove all of these or change to LOGD before release
    LOGI("qwf: init overview");

    toggle_overview.set_handler(toggle_overview_cb);
    output->connect(&view_disappeared);

    input_grab = std::make_unique<wf::input_grab_t>("qwf-overview", output, nullptr, this, nullptr);
    grab_interface.cancel = [=]() { deinit_overview(); };
  }

  void handle_pointer_button(const wlr_pointer_button_event &event) override {
    // TODO allow pointer events for interacting with waybar
    // probably need to change grab_input for that
    // TODO slightly enlarge hovered view to show it's gonna be active
    if (event.button == BTN_LEFT && event.state == WL_POINTER_BUTTON_STATE_RELEASED) {
      auto cursor = wf::get_core().get_cursor_position();

      // views are already sorted with the most recent one first
      for (auto &sv : views) {
        auto transform = sv.view->get_transformed_node()
                             ->get_transformer<wf::scene::view_2d_transformer_t>(window_transformer_name);
        assert(transform);
        auto g = transform->get_bounding_box();

        if (cursor.x < g.x || cursor.x > g.x + g.width || cursor.y < g.y || cursor.y > g.y + g.height) {
          continue;
        } else {
          // TODO it only focuses it once the close transition has stopped running
          // probably because the whole view is "locked" and regular inputs don't work?
          wf::view_bring_to_front(sv.view);
          wf::get_core().default_wm->focus_raise_view(sv.view);
          // currently only close when clicking directly on a view
          handle_overview_close();
          break;
        }
      }
    }
  }

  wf::ipc_activator_t::handler_t toggle_overview_cb = [=](wf::output_t *output, wayfire_view) {
    // TODO add a flag that the animation is in progress so we don't trigger things too quickly
    // TODO display overlay elements such as waybar?
    if (active) {
      return handle_overview_close();
    } else {
      return handle_overview_open();
    }
  };

  wf::effect_hook_t pre_hook = [=]() {
    dim_background(background_dim);
    wf::scene::damage_node(render_node, render_node->get_bounding_box());

    if (!duration.running()) {
      if (!active) {
        deinit_overview();
      }
    }
  };

  wf::signal::connection_t<wf::view_disappeared_signal> view_disappeared =
      [=](wf::view_disappeared_signal *ev) {
        if (auto toplevel = toplevel_cast(ev->view)) {
          handle_view_removed(toplevel);
        }
      };

  void handle_view_removed(wayfire_toplevel_view view) {
    // not running at all, don't care
    if (!output->is_plugin_active(grab_interface.name)) {
      return;
    }

    bool need_action = false;
    for (const auto &sv : views) {
      need_action |= (sv.view == view);
    }

    // don't do anything if we're not using this view
    if (!need_action) {
      return;
    }

    if (active) {
      arrange();
    } else {
      cleanup_views([=](QWFOverviewView &sv) { return sv.view == view; });
    }
  }

  bool handle_overview_open() {
    if (get_workspace_views().empty()) {
      return false;
    }

    // if we haven't grabbed, then we haven't setup anything
    if (!output->is_plugin_active(grab_interface.name)) {
      if (!init_overview()) {
        return false;
      }
    }

    // maybe we're still animating the exit animation from a previous overview activation?
    if (!active) {
      LOGI("qwf: opening overview");

      active = true;
      input_grab->grab_input(wf::scene::layer::OVERLAY);
      arrange();
    }

    return true;
  }

  bool handle_overview_close() {
    LOGI("qwf: closing overview");

    dearrange();
    input_grab->ungrab_input();
    active = false;

    return true;
  }

  /* Sets up basic hooks needed while overview works and/or displays animations.
   * Also lower any fullscreen views that are active */
  bool init_overview() {
    if (!output->activate_plugin(&grab_interface)) {
      return false;
    }

    output->render->add_effect(&pre_hook, wf::OUTPUT_EFFECT_PRE);

    render_node = std::make_shared<overview_render_node_t>(this);
    wf::scene::add_front(wf::get_core().scene(), render_node);
    return true;
  }

  /* The reverse of init_overview */
  void deinit_overview() {
    output->deactivate_plugin(&grab_interface);

    output->render->rem_effect(&pre_hook);
    wf::scene::remove_child(render_node);
    render_node = nullptr;

    for (auto &view : output->wset()->get_views()) {
      if (view->has_data("qwf-overview-minimized-showed")) {
        view->erase_data("qwf-overview-minimized-showed");
        wf::scene::set_node_enabled(view->get_root_node(), false);
      }

      view->get_transformed_node()->rem_transformer(window_transformer_name);
      view->get_transformed_node()->rem_transformer(background_transformer_name);
    }

    views.clear();

    wf::scene::update(wf::get_core().scene(), wf::scene::update_flag::INPUT_STATE);
  }

  // returns a list of mapped views
  std::vector<wayfire_toplevel_view> get_workspace_views() const {
    return output->wset()->get_views(wf::WSET_MAPPED_ONLY | wf::WSET_CURRENT_WORKSPACE);
  }

  /* Create the initial arrangement on the screen
   * Also sorts the views so the last focused one is at the front */
  void arrange() {
    // clear views in case that deinit() hasn't been run
    views.clear();

    duration.start();
    background_dim.set(1, background_dim_factor);
    background_dim_duration.start();

    auto ws_views = get_workspace_views();

    if (ws_views.empty()) {
      return;
    }

    for (auto v : ws_views) {
      views.push_back(create_overview_view(v));
    }

    // keep in case windows overlap for some reason and we need to focus the top one
    // the computed layout does not modify the order
    std::sort(views.begin(), views.end(), [](QWFOverviewView &a, QWFOverviewView &b) {
      return wf::get_focus_timestamp(a.view) > wf::get_focus_timestamp(b.view);
    });

    // TODO _adjustSpacingAndPadding to set row_spacing and col_spacing

    compute_layout(1);
  }

  /* This adapts the code from the Gnome "Activity Overview" to position the windows into rows and columns.
   * https://gitlab.gnome.org/GNOME/gnome-shell/-/blob/77d3a582abb336930c6c51725be2ad62794fe1e2/js/ui/workspace.js */
  void compute_layout(int num_rows) {
    assert(num_rows > 0);

    float total_width = 0.0;
    for (const auto &sv : views) {
      const auto g = sv.view->get_geometry();
      total_width += g.width * compute_window_scale(g);
    }

    float ideal_row_width = total_width / (float)num_rows;

    // TODO instead sort in the opposite direction so we can do pop_back to better iterate over them?

    // generate indices for each window and sort them by the vertical window position
    std::vector<int> view_idxs(views.size());
    std::iota(view_idxs.begin(), view_idxs.end(), 0);
    sort_vertical(view_idxs);

    std::vector<QWFOverviewRow> rows;
    int view_idx = 0;
    for (int i = 0; i < num_rows; i++) {
      QWFOverviewRow row{};

      for (; view_idx < (int)view_idxs.size(); view_idx++) {
        const auto g = views[view_idx].view->get_geometry();
        // TODO we're calculating this multiple times for no reason
        float scale = compute_window_scale(g);
        float width = g.width * scale;
        float height = g.height * scale;
        row.full_height = std::max(row.full_height, height);

        if (keep_same_row(row, width, ideal_row_width) || (i == num_rows - 1)) {
          row.view_idxs.push_back(view_idx);
          row.full_width += width;
        } else {
          break;
        }
      }

      rows.push_back(row);
    }

    float grid_height = 0.0;
    QWFOverviewRow *max_row = nullptr;
    for (auto &row : rows) {
      sort_horizontal(row.view_idxs);

      if (!max_row || row.full_width > max_row->full_width) {
        max_row = &row;
      }
      grid_height += row.full_height;
    }
    int max_columns = max_row->view_idxs.size();
    float grid_width = max_row->full_width;

    float scale, space;
    std::tie(scale, space) = compute_scale_and_space(num_rows, max_columns, grid_width, grid_height);

    // TODO loop with rows and _isBetterScaleAndSpace
    // this is required for trying all row combinations

    // TODO logic from _getWindowSlots()

    compute_window_slots(rows, scale);
  }

  std::tuple<float, float> compute_scale_and_space(int num_rows, int num_cols, float grid_width, float grid_height) {
    const auto screen = output->get_screen_size();

    float h_spacing = (num_cols - 1) * col_spacing;
    float v_spacing = (num_rows - 1) * row_spacing;

    float spaced_width = (float)screen.width - h_spacing;
    float spaced_height = (float)screen.height - v_spacing;

    float h_scale = spaced_width / grid_width;
    float v_scale = spaced_height / grid_height;

    float scale = std::min(std::min(h_scale, v_scale), window_preview_max_scale);

    float scaled_layout_width = grid_width * scale + h_spacing;
    float scaled_layout_height = grid_height * scale + v_spacing;
    float space = (scaled_layout_width * scaled_layout_height) / (screen.width * screen.height);

    return std::make_tuple(scale, space);
  }

  void compute_window_slots(std::vector<QWFOverviewRow> &rows, float scale) {

    compute_row_sizes(rows, scale);

    float height_without_spacing = 0.0;
    for (const auto &row : rows) {
      height_without_spacing += row.height;
    }

    const auto screen = output->get_relative_geometry();

    float v_spacing = (rows.size() - 1) * row_spacing;
    float extra_v_scale = std::min(1.0f, (screen.height - v_spacing) / height_without_spacing);

    float compensation = 0.0;

    float y = 0.0;
    for (auto &row : rows) {
      float h_spacing = (row.view_idxs.size() - 1) * col_spacing;
      float width_without_spacing = row.width - h_spacing;
      float extra_h_scale = std::min(1.0f, (screen.width - h_spacing) / width_without_spacing);

      if (extra_h_scale < extra_v_scale) {
        row.extra_scale = extra_h_scale;
        compensation += (extra_v_scale - extra_h_scale) * row.height;
      } else {
        row.extra_scale = extra_v_scale;
      }

      row.x = screen.x + std::max(0.0f, (screen.width - (width_without_spacing * row.extra_scale + h_spacing)) / 2.0f);
      row.y = screen.y + y + std::max(0.0f, (screen.height - (height_without_spacing + v_spacing)) / 2.0f);
      y += row.height * row.extra_scale + row_spacing;
    }

    compensation /= 2.0;

    for (auto &row : rows) {
      float row_y = row.y + compensation;
      float row_height = row.height * row.extra_scale;

      float x = row.x;
      for (const int view_idx : row.view_idxs) {
        auto &sv = views[view_idx];
        const auto view_g = sv.view->get_geometry();

        float cell_scale = scale * compute_window_scale(view_g) * row.extra_scale;
        float cell_width = view_g.width * cell_scale;
        float cell_height = view_g.height * cell_scale;

        {
          float transform_scale = std::min(cell_scale, window_preview_max_scale);

          // geometry_t uses int, since we want to align with the pixel grid anyway it's fine that we lose precision here
          auto transform_g = wf::geometry_t{};
          transform_g.width = cell_width;
          transform_g.height = cell_height;
          transform_g.x = x;
          if (rows.size() == 1) {
            transform_g.y = row_y + (row_height - cell_height) / 2.0;
          } else {
            transform_g.y = row_y + row_height - cell_height;
          }

          auto offset = get_center(transform_g) - get_center(view_g);

          // TODO when trying for the best layout we probably have to extract this so the transition is only modified once
          // floor values to align with pixel grid
          reposition(sv, offset.round_down(), transform_scale);
        }

        x += cell_width + col_spacing;
      }
    }
  }

  float compute_window_scale(const wf::geometry_t &g) {
    const auto screen = output->get_screen_size();
    float ratio = (float)g.height / (float)screen.height;
    return lerp(1.5, 1.0, ratio);
  }

  float lerp(float a, float b, float t) {
    return a * (1.0 - t) + (b * t);
  }

  void compute_row_sizes(std::vector<QWFOverviewRow> &rows, float scale) {
    for (auto &row : rows) {
      row.width = row.full_width * scale + (row.view_idxs.size() - 1) * col_spacing;
      row.height = row.full_height * scale;
    }
  }

  bool keep_same_row(const QWFOverviewRow &row, float width, float idealRowWidth) {
    if (row.full_width + width <= idealRowWidth) {
      return true;
    }

    float oldRatio = row.full_width / idealRowWidth;
    float newRatio = (row.full_width + width) / idealRowWidth;

    if (std::abs(1 - newRatio) < std::abs(1 - oldRatio)) {
      return true;
    }

    return false;
  }

  void reposition(QWFOverviewView &sv, wf::point_t offset, float scale) {
    sv.attribs.off_x.restart_with_end(offset.x);
    sv.attribs.off_y.restart_with_end(offset.y);
    sv.attribs.scale_x.restart_with_end(scale);
    sv.attribs.scale_y.restart_with_end(scale);
  }

  /* Sort windows horizontally to minimize travel distance.
   * This affects in what order the windows end up in a row. */
  void sort_horizontal(std::vector<int> &view_idxs) {
    std::sort(view_idxs.begin(), view_idxs.end(), [this](int a, int b) {
      auto ag = views[a].view->get_geometry();
      auto bg = views[b].view->get_geometry();
      float a_center_x = ag.x + ag.width / 2.0;
      float b_center_x = bg.x + bg.width / 2.0;
      return a_center_x < b_center_x;
    });
  }

  /* Sort windows vertically to minimize travel distance.
   * This affects what rows the windows get placed in. */
  void sort_vertical(std::vector<int> &view_idxs) {
    std::sort(view_idxs.begin(), view_idxs.end(), [this](int a, int b) {
      auto ag = views[a].view->get_geometry();
      auto bg = views[b].view->get_geometry();
      float a_center_y = ag.y + ag.height / 2.0;
      float b_center_y = bg.y + bg.height / 2.0;
      return a_center_y < b_center_y;
    });
  }

  wf::pointf_t get_center(wf::geometry_t g) {
    return {
        g.x + g.width / 2.0,
        g.y + g.height / 2.0,
    };
  }

  void dearrange() {
    for (auto &sv : views) {
      sv.attribs.off_x.restart_with_end(0);
      sv.attribs.off_y.restart_with_end(0);

      sv.attribs.scale_x.restart_with_end(1.0);
      sv.attribs.scale_y.restart_with_end(1.0);
    }

    background_dim.restart_with_end(1);
    background_dim_duration.start();
    duration.start();
  }

  std::vector<wayfire_view> get_background_views() const {
    return wf::collect_views_from_output(output, {wf::scene::layer::BACKGROUND, wf::scene::layer::BOTTOM});
  }

  std::vector<wayfire_view> get_overlay_views() const {
    return wf::collect_views_from_output(output, {wf::scene::layer::TOP, wf::scene::layer::OVERLAY, wf::scene::layer::DWIDGET});
  }

  void dim_background(float dim) {
    for (auto view : get_background_views()) {
      if (dim == 1.0) {
        view->get_transformed_node()->rem_transformer(
            background_transformer_name);
      } else {
        auto tr =
            wf::ensure_named_transformer<wf::scene::view_3d_transformer_t>(
                view, wf::TRANSFORMER_3D, background_transformer_name,
                view);
        tr->color[0] = tr->color[1] = tr->color[2] = dim;
      }
    }
  }

  QWFOverviewView create_overview_view(wayfire_toplevel_view view) {
    /* we add a view transform if there isn't any.
     *
     * Note that a view might be visible on more than 1 place, so damage
     * tracking doesn't work reliably. To circumvent this, we simply damage
     * the whole output */
    if (!view->get_transformed_node()->get_transformer(window_transformer_name)) {
      if (view->minimized) {
        wf::scene::set_node_enabled(view->get_root_node(), true);
        view->store_data(std::make_unique<wf::custom_data_t>(), "qwf-overview-minimized-showed");
      }

      // TODO make this a 3d transformer again so the windows have a z order and don't go through each other
      view->get_transformed_node()->add_transformer(
          std::make_shared<wf::scene::view_2d_transformer_t>(view),
          wf::TRANSFORMER_2D, window_transformer_name);
    }

    QWFOverviewView sw{duration};
    sw.view = view;

    return sw;
  }

  void render_view_scene(wayfire_view view, const wf::render_target_t &buffer) {
    std::vector<wf::scene::render_instance_uptr> instances;
    view->get_transformed_node()->gen_render_instances(instances, [](auto) {});

    wf::scene::render_pass_params_t params;
    params.instances = &instances;
    params.damage = view->get_transformed_node()->get_bounding_box();
    params.reference_output = this->output;
    params.target = buffer;
    wf::scene::run_render_pass(params, 0);
  }

  void render_view(const QWFOverviewView &sv, const wf::render_target_t &buffer) {
    auto transform = sv.view->get_transformed_node()
                         ->get_transformer<wf::scene::view_2d_transformer_t>(window_transformer_name);
    assert(transform);

    LOGI("qwf: render x ", sv.attribs.off_x);

    transform->translation_x = sv.attribs.off_x;
    transform->translation_y = sv.attribs.off_y;

    transform->scale_x = sv.attribs.scale_x;
    transform->scale_y = sv.attribs.scale_y;

    render_view_scene(sv.view, buffer);
  }

  void render(const wf::render_target_t &fb) {
    OpenGL::render_begin(fb);
    OpenGL::clear({0, 0, 0, 1});
    OpenGL::render_end();

    for (auto view : get_background_views()) {
      render_view_scene(view, fb);
    }

    for (auto view : views) {
      render_view(view, fb);
    }

    for (auto view : get_overlay_views()) {
      render_view_scene(view, fb);
    }
  }

  /* Delete all views matching the given criteria */
  void cleanup_views(std::function<bool(QWFOverviewView &)> criteria) {
    auto it = views.begin();
    while (it != views.end()) {
      if (criteria(*it)) {
        it = views.erase(it);
      } else {
        ++it;
      }
    }
  }

  void fini() override {
    if (output->is_plugin_active(grab_interface.name)) {
      input_grab->ungrab_input();
      deinit_overview();
    }
  }
};

DECLARE_WAYFIRE_PLUGIN(wf::per_output_plugin_t<QWFOverview>);
