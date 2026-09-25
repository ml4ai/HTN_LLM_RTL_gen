#pragma once
#include <graphviz/gvc.h>
#include <string>
#include "typedefs.h"

inline void set_property(Agnode_t *node,
                  std::string property_name,
                  std::string property_value) {
  agsafeset(node,
            const_cast<char *>(property_name.c_str()),
            const_cast<char *>(property_value.c_str()),
            const_cast<char *>(""));
}

inline void set_property(Agedge_t *edge,
                  std::string property_name,
                  std::string property_value) {
  agsafeset(edge,
            const_cast<char *>(property_name.c_str()),
            const_cast<char *>(property_value.c_str()),
            const_cast<char *>(""));
}

inline void set_property(Agraph_t *g,
                  int kind,
                  std::string property_name,
                  std::string property_value) {
  agattr(g,
         kind,
         const_cast<char *>(property_name.c_str()),
         const_cast<char *>(property_value.c_str()));
}

inline Agnode_t *add_node(Agraph_t *g, std::string node_name) {
  return agnode(g, const_cast<char *>(node_name.c_str()), 1);
}
 
inline void  build_graph(Agraph_t *g, 
                  DomainDef& domain, 
                  TaskTree& t,
                  int w,
                  std::unordered_map<std::string,std::string>& action_map) {
  std::string tmp = std::to_string(w);
  Agnode_t *n = add_node(g,tmp);
  set_property(n,"label",t[w].token);
  if (domain.actions.contains(t[w].task)) {
    set_property(n,"shape","rectangle");
    set_property(n,"color","darkorange");
    action_map[t[w].token+"_"+tmp] = tmp;
  }
  for (int i = t[w].children.size() - 1; i >= 0; i--) {
    build_graph(g,domain,t,t[w].children[i],action_map);
    std::string ctmp = std::to_string(t[w].children[i]);
    Agnode_t *m = add_node(g,ctmp);
    //Everything below draws from m, so a node Graphviz could not make ends
    //this child: the check used to guard only the next edge (8.21).
    if (m == NULL) {
      continue;
    }
    Agedge_t *e = agedge(g,n,m,0,1);
    set_property(e,"style","dotted");
    for (auto& o : t[t[w].children[i]].outgoing) {
      Agnode_t *u;
      Agedge_t *p;
      std::string otmp = std::to_string(o);
      u = add_node(g,otmp);
      p = agedge(g,m,u,0,1);
      set_property(p,"color","dodgerblue");
    }
  }
  return;
}

inline void generate_graph(std::vector<std::string>& plan,std::vector<int> roots,DomainDef& domain, TaskTree& t, std::string filename) {
  Agraph_t *g;
  GVC_t *gvc;
  gvc = gvContext();
  g = agopen(const_cast<char*>("g"), Agdirected,NULL);
  std::unordered_map<std::string,std::string> action_map;
  for (auto const& root : roots) {
    build_graph(g,domain,t,root,action_map);
  }
  //Every plan step is an action node of the tree. operator[] used to answer a
  //step with no node by drawing a new one named "" (8.21); that would be a
  //planner bug, so it is reported as one.
  auto node_of = [&](std::string const& step) {
    auto it = action_map.find(step);
    if (it == action_map.end()) {
      throw std::logic_error("plan step "+step+" has no node in the task tree");
    }
    return it->second;
  };
  for (size_t i = 1; i < plan.size(); i++) {
    Agnode_t *v = add_node(g,node_of(plan[i-1]));
    Agnode_t *w = add_node(g,node_of(plan[i]));
    Agedge_t *e = agedge(g,v,w,0,1);
    set_property(e,"color","red");
  }
  gvLayout(gvc,g,"dot");
  gvRenderFilename(gvc,g,"png", const_cast<char*>(filename.c_str()));
  gvFreeLayout(gvc, g);
  agclose(g);
  gvFreeContext(gvc);
}
