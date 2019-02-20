#include "database.h"
#include <iostream>
#include <sstream>
#include <sqlite3.h>
#include <unistd.h>
#include <cstdlib>

#define VISIBLE 0
#define INPUT 1
#define OUTPUT 2
#define INDEXES 3

struct Database::detail {
  sqlite3 *db;
  sqlite3_stmt *add_target;
  sqlite3_stmt *del_target;
  sqlite3_stmt *begin_txn;
  sqlite3_stmt *commit_txn;
  sqlite3_stmt *insert_job;
  sqlite3_stmt *insert_tree;
  sqlite3_stmt *insert_log;
  sqlite3_stmt *wipe_file;
  sqlite3_stmt *insert_file;
  sqlite3_stmt *update_file;
  sqlite3_stmt *get_log;
  sqlite3_stmt *get_tree;
  sqlite3_stmt *set_runtime;
  sqlite3_stmt *detect_overlap;
  sqlite3_stmt *delete_overlap;
  sqlite3_stmt *find_prior;
  sqlite3_stmt *update_prior;
  sqlite3_stmt *find_owner;
  sqlite3_stmt *fetch_hash;
  long run_id;
  detail() : db(0), add_target(0), del_target(0), begin_txn(0), commit_txn(0), insert_job(0),
             insert_tree(0), insert_log(0), wipe_file(0), insert_file(0), update_file(0),
             get_log(0), get_tree(0), set_runtime(0), detect_overlap(0), delete_overlap(0),
             find_prior(0), update_prior(0), find_owner(0), fetch_hash(0) { }
};

Database::Database() : imp(new detail) { }
Database::~Database() { close(); }

std::string Database::open() {
  if (imp->db) return "";
  int ret;

  ret = sqlite3_open_v2("wake.db", &imp->db, SQLITE_OPEN_READWRITE, 0);
  if (ret != SQLITE_OK) {
    if (!imp->db) return "sqlite3_open: out of memory";
    std::string out = sqlite3_errmsg(imp->db);
    close();
    return out;
  }

  const char *schema_sql =
    "pragma journal_mode=wal;"
    "pragma synchronous=0;"
    "pragma locking_mode=exclusive;"
    "pragma foreign_keys=on;"
    "create table if not exists targets("
    "  expression text primary key);"
    "create table if not exists runs("
    "  run_id integer primary key,"
    "  time   text    not null default current_timestamp);"
    "create table if not exists files("
    "  file_id  integer primary key,"
    "  path     text    not null,"
    "  hash     text    not null,"
    "  modified integer not null);"
    "create unique index if not exists filenames on files(path);"
    "create table if not exists jobs("
    "  job_id      integer primary key,"
    "  run_id      integer not null references runs(run_id),"
    "  use_id      integer not null references runs(run_id),"
    "  directory   text    not null,"
    "  commandline text    not null,"
    "  environment text    not null,"
    "  stack       text    not null,"
    "  stdin       text,"    // might point outside the workspace
    "  keep        integer," // 0=false, 1=true
    "  time        text,"
    "  status      integer,"
    "  runtime     real);"
    "create index if not exists job on jobs(directory, commandline, environment, stdin);"
    "create table if not exists filetree("
    "  access  integer not null," // 0=visible, 1=input, 2=output, 3=indexes
    "  job_id  integer not null references jobs(job_id) on delete cascade,"
    "  file_id integer not null references files(file_id),"
    "  primary key(job_id, access, file_id) on conflict ignore);"
    "create index if not exists filesearch on filetree(file_id, access);"
    "create table if not exists log("
    "  job_id     integer not null references jobs(job_id) on delete cascade,"
    "  descriptor integer not null," // 1=stdout, 2=stderr"
    "  seconds    real    not null," // seconds after job start
    "  output     text    not null);"
    "create index if not exists logorder on log(job_id, descriptor, seconds);";
  char *fail;
  ret = sqlite3_exec(imp->db, schema_sql, 0, 0, &fail);
  if (ret != SQLITE_OK) {
    std::string out = fail;
    sqlite3_free(fail);
    close();
    return out;
  }

  // prepare statements
  const char *sql_add_target = "insert into targets(expression) values(?)";
  const char *sql_del_target = "delete from targets where expression=?";
  const char *sql_begin_txn = "begin transaction";
  const char *sql_commit_txn = "commit transaction";
  const char *sql_insert_job =
    "insert into jobs(run_id, use_id, directory, commandline, environment, stack, stdin) "
    "values(?, ?1, ?, ?, ?, ?, ?)";
  const char *sql_insert_tree =
    "insert into filetree(access, job_id, file_id) "
    "values(?, ?, (select file_id from files where path=?))";
  const char *sql_insert_log =
    "insert into log(job_id, descriptor, seconds, output) "
    "values(?, ?, ?, ?)";
  const char *sql_wipe_file =
    "delete from jobs where job_id in"
    " (select distinct job_id from filetree"
    "  where file_id in (select file_id from files where path=? and hash<>?) and access=1)";
  const char *sql_insert_file =
    "insert or ignore into files(hash, modified, path) values (?, ?, ?)";
  const char *sql_update_file =
    "update files set hash=?, modified=? where path=?";
  const char *sql_get_log =
    "select output from log where job_id=? and descriptor=? order by seconds";
  const char *sql_get_tree =
    "select f.path, f.hash from filetree t, files f"
    " where t.job_id=? and t.access=? and f.file_id=t.file_id";
  const char *sql_set_runtime =
    "update jobs set time=current_timestamp, keep=?, status=?, runtime=? where job_id=?";
  const char *sql_detect_overlap =
    "select distinct f.path from filetree t, files f"
    " where t.access=2 and t.job_id<>?1 and t.file_id in "
    " (select file_id from filetree where job_id=?1 and access=2) and f.file_id=t.file_id";
  const char *sql_delete_overlap =
    "delete from jobs where use_id<>? and job_id in "
    "(select distinct job_id from filetree"
    "  where file_id in (select file_id from filetree where job_id=? and access=2) and access=2)";
  const char *sql_find_prior =
    "select job_id from jobs where "
    "directory=? and commandline=? and environment=? and stdin=? and status=0 and keep=1";
  const char *sql_update_prior =
    "update jobs set use_id=? where job_id=?";
  const char *sql_find_owner =
    "select j.job_id, j.directory, j.commandline, j.environment, j.stack, j.stdin, j.time, j.status, j.runtime"
    " from files f, filetree t, jobs j"
    " where f.path=? and t.file_id=f.file_id and t.access=? and j.job_id=t.job_id";
  const char *sql_fetch_hash =
    "select hash from files where path=? and modified=?;";

#define PREPARE(sql, member)										\
  ret = sqlite3_prepare_v2(imp->db, sql, -1, &imp->member, 0);						\
  if (ret != SQLITE_OK) {										\
    std::string out = std::string("sqlite3_prepare_v2 " #member ": ") + sqlite3_errmsg(imp->db);	\
    close();												\
    return out;												\
  }

  PREPARE(sql_add_target,     add_target);
  PREPARE(sql_del_target,     del_target);
  PREPARE(sql_begin_txn,      begin_txn);
  PREPARE(sql_commit_txn,     commit_txn);
  PREPARE(sql_insert_job,     insert_job);
  PREPARE(sql_insert_tree,    insert_tree);
  PREPARE(sql_insert_log,     insert_log);
  PREPARE(sql_wipe_file,      wipe_file);
  PREPARE(sql_insert_file,    insert_file);
  PREPARE(sql_update_file,    update_file);
  PREPARE(sql_get_log,        get_log);
  PREPARE(sql_get_tree,       get_tree);
  PREPARE(sql_set_runtime,    set_runtime);
  PREPARE(sql_detect_overlap, detect_overlap);
  PREPARE(sql_delete_overlap, delete_overlap);
  PREPARE(sql_find_prior,     find_prior);
  PREPARE(sql_update_prior,   update_prior);
  PREPARE(sql_find_owner,     find_owner);
  PREPARE(sql_fetch_hash,     fetch_hash);

  return "";
}

void Database::close() {
  int ret;

#define FINALIZE(member)						\
  if  (imp->member) {							\
    ret = sqlite3_finalize(imp->member);				\
    if (ret != SQLITE_OK) {						\
      std::cerr << "Could not sqlite3_finalize " << #member		\
                << ": " << sqlite3_errmsg(imp->db) << std::endl;	\
      return;								\
    }									\
  }									\
  imp->member = 0;

  FINALIZE(add_target);
  FINALIZE(del_target);
  FINALIZE(begin_txn);
  FINALIZE(commit_txn);
  FINALIZE(insert_job);
  FINALIZE(insert_tree);
  FINALIZE(insert_log);
  FINALIZE(wipe_file);
  FINALIZE(insert_file);
  FINALIZE(update_file);
  FINALIZE(get_log);
  FINALIZE(get_tree);
  FINALIZE(set_runtime);
  FINALIZE(detect_overlap);
  FINALIZE(delete_overlap);
  FINALIZE(find_prior);
  FINALIZE(update_prior);
  FINALIZE(find_owner);
  FINALIZE(fetch_hash);

  if (imp->db) {
    int ret = sqlite3_close(imp->db);
    if (ret != SQLITE_OK) {
      std::cerr << "Could not close wake.db: " << sqlite3_errmsg(imp->db) << std::endl;
      return;
    }
  }
  imp->db = 0;
}

static int fill_vector(void *data, int cols, char **text, char **colname) {
  (void)colname;
  if (cols >= 1) {
    std::vector<std::string> *vec = reinterpret_cast<std::vector<std::string>*>(data);
    vec->emplace_back(text[0]);
  }
  return 0;
}

static void finish_stmt(const char *why, sqlite3_stmt *stmt) {
  int ret;

  ret = sqlite3_reset(stmt);
  if (ret != SQLITE_OK) {
    std::cerr << why << "; sqlite3_reset: " << sqlite3_errmsg(sqlite3_db_handle(stmt)) << std::endl;
    exit(1);
  }

  ret = sqlite3_clear_bindings(stmt);
  if (ret != SQLITE_OK) {
    std::cerr << why << "; sqlite3_clear_bindings: " << sqlite3_errmsg(sqlite3_db_handle(stmt)) << std::endl;
    exit(1);
  }
}

static void single_step(const char *why, sqlite3_stmt *stmt) {
  int ret;

  ret = sqlite3_step(stmt);
  if (ret != SQLITE_DONE) {
    std::cerr << why << "; sqlite3_step: " << sqlite3_errmsg(sqlite3_db_handle(stmt)) << std::endl;
    std::cerr << "The failing statement was: ";
#if SQLITE_VERSION_NUMBER >= 3014000
    char *tmp = sqlite3_expanded_sql(stmt);
    std::cerr << tmp;
    sqlite3_free(tmp);
#else
    std::cerr << sqlite3_sql(stmt);
#endif
    std::cerr << std::endl;
    exit(1);
  }

  finish_stmt(why, stmt);
}

static void bind_string(const char *why, sqlite3_stmt *stmt, int index, const char *str, size_t len) {
  int ret;
  ret = sqlite3_bind_text(stmt, index, str, len, SQLITE_STATIC);
  if (ret != SQLITE_OK) {
    std::cerr << why << "; sqlite3_bind_text(" << index << "): " << sqlite3_errmsg(sqlite3_db_handle(stmt)) << std::endl;
    exit(1);
  }
}

static void bind_string(const char *why, sqlite3_stmt *stmt, int index, const std::string &x) {
  bind_string(why, stmt, index, x.data(), x.size());
}

static void bind_integer(const char *why, sqlite3_stmt *stmt, int index, long x) {
  int ret;
  ret = sqlite3_bind_int64(stmt, index, x);
  if (ret != SQLITE_OK) {
    std::cerr << why << "; sqlite3_bind_int64(" << index << "): " << sqlite3_errmsg(sqlite3_db_handle(stmt)) << std::endl;
    exit(1);
  }
}

static void bind_double(const char *why, sqlite3_stmt *stmt, int index, double x) {
  int ret;
  ret = sqlite3_bind_double(stmt, index, x);
  if (ret != SQLITE_OK) {
    std::cerr << why << "; sqlite3_bind_double(" << index << "): " << sqlite3_errmsg(sqlite3_db_handle(stmt)) << std::endl;
    exit(1);
  }
}

static std::string rip_column(sqlite3_stmt *stmt, int col) {
  return std::string(
    reinterpret_cast<const char*>(sqlite3_column_text(stmt, col)),
    sqlite3_column_bytes(stmt, col));
}

std::vector<std::string> Database::get_targets() {
  std::vector<std::string> out;
  int ret = sqlite3_exec(imp->db, "select expression from targets;", &fill_vector, &out, 0);
  if (ret != SQLITE_OK)
    std::cerr << "Could not enumerate wake targets: " << sqlite3_errmsg(imp->db) << std::endl;
  return out;
}

void Database::add_target(const std::string &target) {
  const char *why = "Could not add a wake target";
  bind_string(why, imp->add_target, 1, target);
  single_step(why, imp->add_target);
}

void Database::del_target(const std::string &target) {
  const char *why = "Could not remove a wake target";
  bind_string(why, imp->del_target, 1, target);
  single_step(why, imp->del_target);
}

void Database::prepare() {
  std::vector<std::string> out;
  const char *sql = "insert into runs(run_id) values(null);";
  int ret = sqlite3_exec(imp->db, sql, 0, 0, 0);
  if (ret != SQLITE_OK) {
    std::cerr << "Could not enumerate wake targets: " << sqlite3_errmsg(imp->db) << std::endl;
    exit(1);
  }
  imp->run_id = sqlite3_last_insert_rowid(imp->db);
}

void Database::clean(bool verbose) {
  // !!!
}

void Database::begin_txn() {
  single_step("Could not begin a transaction", imp->begin_txn);
}

void Database::end_txn() {
  single_step("Could not commit a transaction", imp->commit_txn);
}

bool Database::reuse_job(
  const std::string &directory,
  const std::string &stdin,
  const std::string &environment,
  const std::string &commandline,
  const std::string &visible,
  long *job)
{
  const char *why = "Could not check for a cached job";
  begin_txn();
  bind_string (why, imp->find_prior, 1, directory);
  bind_string (why, imp->find_prior, 2, commandline);
  bind_string (why, imp->find_prior, 3, environment);
  bind_string (why, imp->find_prior, 4, stdin);
  bool prior = sqlite3_step(imp->find_prior) == SQLITE_ROW;
  if (prior) *job = sqlite3_column_int(imp->find_prior, 0);
  finish_stmt (why, imp->find_prior);

  if (!prior) {
    end_txn();
    return false;
  }

  bool exist = true;
  bind_integer(why, imp->get_tree, 1, *job);
  bind_integer(why, imp->get_tree, 2, OUTPUT);
  while (sqlite3_step(imp->get_tree) == SQLITE_ROW) {
    std::string path = rip_column(imp->get_tree, 0);
    if (access(path.c_str(), R_OK) != 0) exist = false;
  }
  finish_stmt(why, imp->get_tree);

  if (exist) {
    bind_integer(why, imp->update_prior, 1, imp->run_id);
    bind_integer(why, imp->update_prior, 2, *job);
    single_step (why, imp->update_prior);
  }

  end_txn();

  return exist;
}

void Database::insert_job(
  const std::string &directory,
  const std::string &stdin,
  const std::string &environment,
  const std::string &commandline,
  const std::string &stack,
  long  *job)
{
  const char *why = "Could not insert a job";
  begin_txn();
  bind_integer(why, imp->insert_job, 1, imp->run_id);
  bind_string (why, imp->insert_job, 2, directory);
  bind_string (why, imp->insert_job, 3, commandline);
  bind_string (why, imp->insert_job, 4, environment);
  bind_string (why, imp->insert_job, 5, stack);
  bind_string (why, imp->insert_job, 6, stdin);
  single_step (why, imp->insert_job);
  end_txn();
  *job = sqlite3_last_insert_rowid(imp->db);
}

void Database::finish_job(long job, const std::string &inputs, const std::string &outputs, bool keep, int status, double runtime) {
  const char *why = "Could not save job inputs and outputs";
  begin_txn();
  bind_integer(why, imp->set_runtime, 1, keep?1:0);
  bind_integer(why, imp->set_runtime, 2, status);
  bind_double (why, imp->set_runtime, 3, runtime);
  bind_integer(why, imp->set_runtime, 4, job);
  single_step (why, imp->set_runtime);
  const char *tok = inputs.c_str();
  const char *end = tok + inputs.size();
  for (const char *scan = tok; scan != end; ++scan) {
    if (*scan == 0 && scan != tok) {
      bind_integer(why, imp->insert_tree, 1, INPUT);
      bind_integer(why, imp->insert_tree, 2, job);
      bind_string (why, imp->insert_tree, 3, tok, scan-tok);
      single_step (why, imp->insert_tree);
      tok = scan+1;
    }
  }
  tok = outputs.c_str();
  end = tok + outputs.size();
  for (const char *scan = tok; scan != end; ++scan) {
    if (*scan == 0 && scan != tok) {
      bind_integer(why, imp->insert_tree, 1, OUTPUT);
      bind_integer(why, imp->insert_tree, 2, job);
      bind_string (why, imp->insert_tree, 3, tok, scan-tok);
      single_step (why, imp->insert_tree);
      tok = scan+1;
    }
  }

  bind_integer(why, imp->delete_overlap, 1, imp->run_id);
  bind_integer(why, imp->delete_overlap, 2, job);
  single_step (why, imp->delete_overlap);

  bool fail = false;
  bind_integer(why, imp->detect_overlap, 1, job);
  while (sqlite3_step(imp->detect_overlap) == SQLITE_ROW) {
    std::cerr << "File output by multiple Jobs: " << rip_column(imp->detect_overlap, 0) << std::endl;
    fail = true;
  }
  finish_stmt(why, imp->detect_overlap);

  end_txn();

  if (fail) exit(1);
}

std::vector<FileReflection> Database::get_tree(int kind, long job)  {
  std::vector<FileReflection> out;
  const char *why = "Could not read job tree";
  bind_integer(why, imp->get_tree, 1, job);
  bind_integer(why, imp->get_tree, 2, kind);
  while (sqlite3_step(imp->get_tree) == SQLITE_ROW)
    out.emplace_back(rip_column(imp->get_tree, 0), rip_column(imp->get_tree, 1));
  finish_stmt(why, imp->get_tree);
  return out;
}

void Database::save_output(long job, int descriptor, const char *buffer, int size, double runtime) {
  const char *why = "Could not save job output";
  bind_integer(why, imp->insert_log, 1, job);
  bind_integer(why, imp->insert_log, 2, descriptor);
  bind_double (why, imp->insert_log, 3, runtime);
  bind_string (why, imp->insert_log, 4, buffer, size);
  single_step (why, imp->insert_log);
}

std::string Database::get_output(long job, int descriptor) {
  std::stringstream out;
  const char *why = "Could not read job output";
  bind_integer(why, imp->get_log, 1, job);
  bind_integer(why, imp->get_log, 2, descriptor);
  while (sqlite3_step(imp->get_log) == SQLITE_ROW) {
    out.write(
      reinterpret_cast<const char*>(sqlite3_column_text(imp->get_log, 0)),
      sqlite3_column_bytes(imp->get_log, 0));
  }
  finish_stmt(why, imp->get_log);
  return out.str();
}

void Database::add_hash(const std::string &file, const std::string &hash, long modified) {
  const char *why = "Could not insert a hash";
  begin_txn();
  bind_string (why, imp->wipe_file, 1, file);
  bind_string (why, imp->wipe_file, 2, hash);
  single_step (why, imp->wipe_file);
  bind_string (why, imp->update_file, 1, hash);
  bind_integer(why, imp->update_file, 2, modified);
  bind_string (why, imp->update_file, 3, file);
  single_step (why, imp->update_file);
  bind_string (why, imp->insert_file, 1, hash);
  bind_integer(why, imp->insert_file, 2, modified);
  bind_string (why, imp->insert_file, 3, file);
  single_step (why, imp->insert_file);
  end_txn();
}

std::string Database::get_hash(const std::string &file, long modified) {
  std::string out;
  const char *why = "Could not fetch a hash";
  bind_string (why, imp->fetch_hash, 1, file);
  bind_integer(why, imp->fetch_hash, 2, modified);
  if (sqlite3_step(imp->fetch_hash) == SQLITE_ROW)
    out = rip_column(imp->fetch_hash, 0);
  finish_stmt(why, imp->fetch_hash);
  return out;
}

static std::vector<std::string> chop_null(const std::string &str) {
  std::vector<std::string> out;
  const char *tok = str.c_str();
  const char *end = tok + str.size();
  for (const char *scan = tok; scan != end; ++scan) {
    if (*scan == 0 && scan != tok) {
      out.emplace_back(tok, scan-tok);
      tok = scan+1;
    }
  }
  return out;
}

std::vector<JobReflection> Database::explain(const std::string &file, int use) {
  const char *why = "Could not explain file";
  std::vector<JobReflection> out;

  begin_txn();
  bind_string (why, imp->find_owner, 1, file);
  bind_integer(why, imp->find_owner, 2, use);
  while (sqlite3_step(imp->find_owner) == SQLITE_ROW) {
    out.resize(out.size()+1);
    JobReflection &desc = out.back();
    desc.job = sqlite3_column_int64(imp->find_owner, 0);
    desc.directory = rip_column(imp->find_owner, 1);
    desc.commandline = chop_null(rip_column(imp->find_owner, 2));
    desc.environment = chop_null(rip_column(imp->find_owner, 3));
    desc.stack = rip_column(imp->find_owner, 4);
    desc.stdin = rip_column(imp->find_owner, 5);
    desc.time = rip_column(imp->find_owner, 6);
    desc.status = sqlite3_column_int64(imp->find_owner, 7);
    desc.runtime = sqlite3_column_double(imp->find_owner, 8);
    if (desc.stdin.empty()) desc.stdin = "/dev/null";
    // inputs
    bind_integer(why, imp->get_tree, 1, desc.job);
    bind_integer(why, imp->get_tree, 2, INPUT);
    while (sqlite3_step(imp->get_tree) == SQLITE_ROW)
      desc.inputs.emplace_back(
        rip_column(imp->get_tree, 0),
        rip_column(imp->get_tree, 1));
    finish_stmt(why, imp->get_tree);
    // outputs
    bind_integer(why, imp->get_tree, 1, desc.job);
    bind_integer(why, imp->get_tree, 2, OUTPUT);
    while (sqlite3_step(imp->get_tree) == SQLITE_ROW)
      desc.outputs.emplace_back(
        rip_column(imp->get_tree, 0),
        rip_column(imp->get_tree, 1));
    finish_stmt(why, imp->get_tree);
  }
  finish_stmt(why, imp->find_owner);
  end_txn();

  return out;
}
