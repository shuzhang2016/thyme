// Code generated for the ports fixture graph. DO NOT EDIT.
// Hand logic belongs in src/nodes/.
// gen_graph 3.11.1 (output format 3)
  if (std::strcmp(node_id, "prod") == 0) {
    thyme::ThreadPlacement p{};
    p.priority = 89;
    t->SetPlacement(p);
  } else if (std::strcmp(node_id, "cons") == 0) {
    thyme::ThreadPlacement p{};
    p.priority = 89;
    t->SetPlacement(p);
  } else if (std::strcmp(node_id, "health_reporter") == 0) {
    thyme::ThreadPlacement p{};
    p.priority = 89;
    t->SetPlacement(p);
  }
