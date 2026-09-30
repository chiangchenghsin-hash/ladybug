CREATE NODE TABLE nodes(id INT32, name STRING, PRIMARY KEY(id)) WITH (storage = '.', format = 'icebug-disk');
CREATE REL TABLE edges(FROM nodes TO nodes, weight INT32) WITH (storage = '.', format = 'icebug-disk');
