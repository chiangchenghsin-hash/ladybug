CREATE NODE TABLE city(id INT32, name STRING, population INT64, PRIMARY KEY(id)) WITH (storage = '.', format = 'icebug-disk');
CREATE NODE TABLE user(id INT32, name STRING, age INT64, PRIMARY KEY(id)) WITH (storage = '.', format = 'icebug-disk');
CREATE REL TABLE follows(FROM user TO user, since INT32) WITH (storage = '.', format = 'icebug-disk');
CREATE REL TABLE livesin(FROM user TO city) WITH (storage = '.', format = 'icebug-disk');
