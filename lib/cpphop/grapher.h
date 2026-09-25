#pragma once
//Draws the task hierarchy behind a plan with Graphviz (planner_doc.md 8.22).
//
//What the drawing shows, and why each choice:
//  * compound tasks as rounded boxes with the method that decomposed them
//    ("by m_deliver"), actions as square boxes numbered by plan step;
//  * the synthesised method-precondition checks (the __mprec_ actions of 8.16)
//    left out: they are search steps, not plan steps, and were a third of the
//    nodes. Orderings through one are joined up, not dropped;
//  * only decomposition edges decide the layout, so a node's height is its
//    depth; every action sits on one bottom row, so the plan reads left to
//    right along it, joined by red plan-order arrows;
//  * ordering constraints dashed, and only those no other chain implies;
//  * colour by the top-level task each node serves, or by the object of a
//    chosen type in its arguments (the agent, say), or none;
//  * a caption naming the run and saying what each mark means;
//  * PNG, SVG (with a tooltip on each node), PDF or DOT, by file extension.
#include <graphviz/gvc.h>
#include <algorithm>
#include <climits>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "typedefs.h"

//What the caption says and how nodes are coloured. Everything is optional.
struct GraphOptions {
  std::string title;                   //first caption line: the run
  std::vector<std::string> facts;      //second caption line, joined with dots
  //"top" colours each node by the top-level task it serves; "none" leaves
  //them uncoloured; anything else is a type name, and a node takes the colour
  //of the first argument of that type (or a subtype), e.g. "player".
  std::string colour_by = "top";
  //The problem's objects with their types, needed to colour by type.
  Objects const* objects = nullptr;
};

namespace task_graph {

inline void set_property(void* obj, std::string const& name, std::string const& value) {
  agsafeset(obj, const_cast<char*>(name.c_str()), value.c_str(), "");
}

//An HTML-like label, given without the < > that delimit one in DOT source:
//through the API those are not part of the value. The string agstrdup_html
//makes is reference-counted in the graph's string pool and released with the
//graph; it is not freed here because agstrfree's signature changed between
//Graphviz releases.
inline void set_html(Agraph_t* g, void* obj, std::string const& name, std::string const& html) {
  char* h = agstrdup_html(g, html.c_str());
  agsafeset(obj, const_cast<char*>(name.c_str()), h, "");
}

inline std::string escape(std::string const& s) {
  std::string out;
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out += c;
    }
  }
  return out;
}

//"(deliver package_0 city_loc_0)" -> {"deliver","package_0","city_loc_0"}
inline std::vector<std::string> split_token(std::string const& token) {
  std::vector<std::string> parts;
  std::istringstream in(token.size() >= 2 ? token.substr(1, token.size() - 2) : token);
  std::string w;
  while (in >> w) {
    parts.push_back(w);
  }
  return parts;
}

//The output format a file name asks for.
inline std::string format_of(std::string const& filename) {
  auto dot = filename.rfind('.');
  std::string ext = dot == std::string::npos ? "" : filename.substr(dot + 1);
  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
  if (ext == "png" || ext == "svg" || ext == "pdf") return ext;
  if (ext == "dot" || ext == "gv") return "dot";
  throw std::invalid_argument("graph file " + filename +
                              ": use a .png, .svg, .pdf, .dot or .gv extension");
}

//Fill and line colours; a node's group indexes both.
inline char const* const fills[] = {"#dbeafe", "#fde68a", "#bbf7d0", "#fbcfe8",
                                    "#ddd6fe", "#fed7aa", "#a5f3fc", "#e5e7eb"};
inline char const* const lines[] = {"#1d4ed8", "#b45309", "#15803d", "#be185d",
                                    "#6d28d9", "#c2410c", "#0e7490", "#4b5563"};
inline constexpr int n_colours = 8;

} // namespace task_graph

inline void generate_graph(std::vector<std::string>& plan,
                           std::vector<int> roots,
                           DomainDef& domain,
                           TaskTree& t,
                           std::string filename,
                           GraphOptions const& opts = {}) {
  using namespace task_graph;
  std::string format = format_of(filename);

  auto is_action = [&](int id) { return domain.actions.contains(t.at(id).task); };
  auto is_check = [&](int id) {
    auto it = domain.actions.find(t.at(id).task);
    return it != domain.actions.end() && it->second.is_artificial();
  };

  //Plan steps, 1-based. A plan entry is "(token)_<id>" (see expansion).
  std::map<int,int> step;
  for (size_t i = 0; i < plan.size(); i++) {
    auto u = plan[i].rfind('_');
    int id = -1;
    try {
      id = u == std::string::npos ? -1 : std::stoi(plan[i].substr(u + 1));
    }
    catch (std::exception const&) {}
    if (id < 0 || !t.contains(id)) {
      throw std::logic_error("plan step " + plan[i] + " has no node in the task tree");
    }
    step[id] = (int)i + 1;
  }

  //The visible tree: everything but the synthesised checks.
  std::vector<int> order;             //nodes, parents before children
  std::map<int,std::vector<int>> kids;
  std::map<int,int> group;            //colour group, -1 for none
  std::map<int,int> first_step;       //earliest plan step at or under a node
  std::function<int(int)> first_of = [&](int id) {
    int f = step.contains(id) ? step[id] : INT_MAX;
    for (int c : t.at(id).children) {
      if (!is_check(c)) f = std::min(f, first_of(c));
    }
    return first_step[id] = f;
  };
  for (int r : roots) first_of(r);
  std::function<void(int)> walk = [&](int id) {
    order.push_back(id);
    std::vector<int> cs;
    for (int c : t.at(id).children) {
      if (!is_check(c)) cs.push_back(c);
    }
    //Children written in plan order. Graphviz starts its layout from the input
    //order; ordering=out would enforce it but, with the flat ordering edges
    //below, mirrors every row instead (8.22).
    std::stable_sort(cs.begin(), cs.end(),
                     [&](int a, int b) { return first_step[a] < first_step[b]; });
    kids[id] = cs;
    for (int c : cs) walk(c);
  };
  for (int r : roots) walk(r);

  //Colour groups.
  std::map<std::string,int> group_of_key;
  auto group_for = [&](std::string const& key) {
    auto it = group_of_key.find(key);
    if (it != group_of_key.end()) return it->second;
    int g = (int)group_of_key.size() % n_colours;
    group_of_key[key] = g;
    return g;
  };
  for (int id : order) group[id] = -1;
  if (opts.colour_by == "top") {
    std::vector<int> tops;
    if (roots.size() == 1) tops = kids[roots[0]]; else tops = roots;
    std::function<void(int,int)> paint = [&](int id, int g) {
      group[id] = g;
      for (int c : kids[id]) paint(c, g);
    };
    for (int top : tops) paint(top, group_for(t.at(top).token));
  }
  else if (opts.colour_by != "none") {
    //By the first argument whose object is of the chosen type or a subtype.
    int want = domain.typetree.find_type(opts.colour_by);
    if (want == -1) {
      throw std::invalid_argument("graph colouring: " + opts.colour_by +
                                  " is not a type of this domain (or use top or none)");
    }
    auto of_type = [&](std::string const& obj) {
      if (!opts.objects || !opts.objects->contains(obj)) return false;
      int ty = domain.typetree.find_type(opts.objects->at(obj));
      return ty == want || (ty != -1 && domain.typetree.types[ty].lineage.contains(want));
    };
    for (int id : order) {
      auto parts = split_token(t.at(id).token);
      for (size_t i = 1; i < parts.size(); i++) {
        if (of_type(parts[i])) {
          group[id] = group_for(parts[i]);
          break;
        }
      }
    }
  }

  //Ordering constraints among visible nodes. A check is contracted out:
  //whatever came before it comes before whatever it came before.
  std::map<int,std::set<int>> after;
  for (auto const& [id,node] : t) {
    for (int o : node.outgoing) {
      if (t.contains(o)) after[id].insert(o);
    }
  }
  bool contracted = true;
  while (contracted) {
    contracted = false;
    for (auto& [a,bs] : after) {
      for (int b : std::vector<int>(bs.begin(), bs.end())) {
        if (t.contains(b) && is_check(b)) {
          bs.erase(b);
          for (int c : after[b]) {
            if (c != a) bs.insert(c);
          }
          contracted = true;
        }
      }
    }
  }
  std::set<int> visible(order.begin(), order.end());
  std::map<int,std::set<int>> ord;
  for (auto const& [a,bs] : after) {
    if (!visible.contains(a)) continue;
    for (int b : bs) {
      if (visible.contains(b)) ord[a].insert(b);
    }
  }
  //Transitive reduction: drop a -> b when b is reachable from a another way.
  auto reachable_without = [&](int a, int b) {
    std::vector<int> todo;
    std::set<int> seen;
    for (int c : ord[a]) {
      if (c != b) todo.push_back(c);
    }
    while (!todo.empty()) {
      int x = todo.back();
      todo.pop_back();
      if (x == b) return true;
      if (!seen.insert(x).second) continue;
      for (int y : ord[x]) todo.push_back(y);
    }
    return false;
  };
  std::vector<std::pair<int,int>> ordering_edges;
  for (auto const& [a,bs] : ord) {
    for (int b : bs) {
      if (!reachable_without(a, b)) ordering_edges.push_back({a, b});
    }
  }

  //Build the graph.
  GVC_t* gvc = gvContext();
  Agraph_t* g = agopen(const_cast<char*>("plan"), Agdirected, NULL);
  set_property(g, "rankdir", "TB");
  set_property(g, "newrank", "true");
  set_property(g, "nodesep", "0.25");
  set_property(g, "ranksep", "0.45");
  set_property(g, "fontname", "Helvetica");
  set_property(g, "labelloc", "t");
  set_property(g, "fontsize", "16");
  //Text at the default 72 dpi is too small to read in a bitmap.
  if (format == "png") {
    set_property(g, "dpi", "110");
  }
  agattr(g, AGNODE, const_cast<char*>("fontname"), "Helvetica");
  agattr(g, AGNODE, const_cast<char*>("fontsize"), "11");
  agattr(g, AGNODE, const_cast<char*>("penwidth"), "1.2");
  agattr(g, AGEDGE, const_cast<char*>("color"), "#9ca3af");
  agattr(g, AGEDGE, const_cast<char*>("arrowsize"), "0.6");

  size_t n_compound = 0, n_actions = 0;
  for (int id : order) (is_action(id) ? n_actions : n_compound)++;
  std::string caption = "<B>" + escape(opts.title.empty() ? "Task hierarchy" : opts.title) + "</B>";
  std::vector<std::string> facts = opts.facts;
  facts.push_back(std::to_string(n_actions) + (n_actions == 1 ? " action" : " actions"));
  facts.push_back(std::to_string(n_compound) + (n_compound == 1 ? " compound task" : " compound tasks"));
  std::string fact_line;
  for (size_t i = 0; i < facts.size(); i++) {
    fact_line += (i ? " &middot; " : "") + escape(facts[i]);
  }
  std::string colour_note = opts.colour_by == "top"  ? " &middot; colour = top-level task"
                          : opts.colour_by == "none" ? ""
                          : " &middot; colour = the " + escape(opts.colour_by) + " involved";
  caption += "<BR/><FONT POINT-SIZE=\"11\">" + fact_line + "</FONT>"
             "<BR/><FONT POINT-SIZE=\"9\" COLOR=\"#555555\">rounded box = compound task, "
             "with the method that decomposed it &middot; square box = action, numbered in "
             "plan order &middot; grey = decomposition &middot; red = plan order &middot; "
             "blue dashed = ordering constraint" + colour_note + "</FONT>";
  set_html(g, g, "label", caption);

  std::map<int,Agnode_t*> node;
  for (int id : order) {
    auto const& tn = t.at(id);
    auto parts = split_token(tn.token);
    std::string head = parts.empty() ? tn.task : parts[0];
    std::string args;
    for (size_t i = 1; i < parts.size(); i++) {
      args += (i > 1 ? ", " : "") + parts[i];
    }
    bool action = is_action(id);
    std::string label = "<TABLE BORDER=\"0\" CELLSPACING=\"0\" CELLPADDING=\"1\">";
    if (action && step.contains(id)) {
      label += "<TR><TD ALIGN=\"LEFT\"><FONT POINT-SIZE=\"9\" COLOR=\"#555555\">step " +
               std::to_string(step[id]) + "</FONT></TD></TR>";
    }
    //The root is the problem's own task network, which the planner names
    //"__<problem>__"; show the problem's name and say what it is.
    bool problem_root = std::find(roots.begin(), roots.end(), id) != roots.end() &&
                        head.size() > 4 && head.rfind("__", 0) == 0 &&
                        head.compare(head.size() - 2, 2, "__") == 0;
    if (problem_root) {
      label += "<TR><TD><B>" + escape(head.substr(2, head.size() - 4)) + "</B></TD></TR>"
               "<TR><TD><FONT POINT-SIZE=\"9\" COLOR=\"#555555\"><I>the problem's task network"
               "</I></FONT></TD></TR>";
    }
    else {
      label += "<TR><TD><B>" + escape(head) + "</B></TD></TR>";
    }
    if (!args.empty()) {
      label += "<TR><TD><FONT POINT-SIZE=\"10\">" + escape(args) + "</FONT></TD></TR>";
    }
    //The problem's own top-level "method" is named after the problem class
    //(":htn"), which says nothing.
    if (!action && !tn.method.empty() && tn.method[0] != ':') {
      label += "<TR><TD><FONT POINT-SIZE=\"9\" COLOR=\"#555555\"><I>by " +
               escape(tn.method) + "</I></FONT></TD></TR>";
    }
    label += "</TABLE>";

    Agnode_t* n = agnode(g, const_cast<char*>(("n" + std::to_string(id)).c_str()), 1);
    node[id] = n;
    set_html(g, n, "label", label);
    set_property(n, "shape", "box");
    set_property(n, "style", action ? "filled" : "rounded,filled");
    int grp = group[id];
    set_property(n, "fillcolor", grp < 0 ? "#f3f4f6" : fills[grp]);
    set_property(n, "color", grp < 0 ? "#374151" : lines[grp]);
    std::string tip = tn.token;
    if (!tn.method.empty() && tn.method[0] != ':') tip += "  by " + tn.method;
    if (step.contains(id)) tip = "step " + std::to_string(step[id]) + ": " + tip;
    set_property(n, "tooltip", tip);
  }

  for (int id : order) {
    for (int c : kids[id]) {
      agedge(g, node[id], node[c], NULL, 1);
    }
  }
  for (auto const& [a,b] : ordering_edges) {
    Agedge_t* e = agedge(g, node[a], node[b], NULL, 1);
    set_property(e, "color", "#3b82f6");
    set_property(e, "style", "dashed");
    set_property(e, "constraint", "false");
    set_property(e, "arrowsize", "0.5");
  }

  //Every action on the bottom row, in plan order.
  std::vector<int> actions;
  for (auto const& [id,s] : step) {
    if (node.contains(id)) actions.push_back(id);
  }
  std::sort(actions.begin(), actions.end(), [&](int a, int b) { return step[a] < step[b]; });
  if (!actions.empty()) {
    Agraph_t* bottom = agsubg(g, const_cast<char*>("plan_row"), 1);
    set_property(bottom, "rank", "sink");
    for (int id : actions) agsubnode(bottom, node[id], 1);
  }
  for (size_t i = 1; i < actions.size(); i++) {
    Agedge_t* e = agedge(g, node[actions[i-1]], node[actions[i]], NULL, 1);
    set_property(e, "color", "#dc2626");
    set_property(e, "penwidth", "1.6");
    set_property(e, "constraint", "false");
  }

  gvLayout(gvc, g, "dot");
  int rc = gvRenderFilename(gvc, g, format.c_str(), filename.c_str());
  gvFreeLayout(gvc, g);
  agclose(g);
  gvFreeContext(gvc);
  if (rc != 0) {
    throw std::runtime_error("Graphviz could not write " + filename);
  }
}
