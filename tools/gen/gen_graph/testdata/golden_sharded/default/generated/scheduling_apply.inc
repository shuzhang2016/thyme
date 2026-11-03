// Code generated for the generated graph. DO NOT EDIT.
// Hand logic belongs in src/nodes/.
// gen_graph 3.11.1 (output format 3)
  if (std::strcmp(node_id, "prod") == 0) {
    thyme::ThreadPlacement p{};
    p.ncpus = 1;
    p.cpus[0] = 0;
    p.priority = 89;
    t->SetPlacement(p);
  }
