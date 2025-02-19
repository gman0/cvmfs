/**
 * This file is part of the CernVM File System.
 */

#include "history_sqlite.h"

using namespace std;  // NOLINT

namespace history {

const std::string SqliteHistory::kPreviousRevisionKey = "previous_revision";


SqliteHistory* SqliteHistory::Open(const std::string &file_name) {
  const bool read_write = false;
  return Open(file_name, read_write);
}


SqliteHistory* SqliteHistory::OpenWritable(const std::string &file_name) {
  const bool read_write = true;
  return Open(file_name, read_write);
}


SqliteHistory* SqliteHistory::Open(const std::string &file_name,
                                   const bool read_write) {
  SqliteHistory *history = new SqliteHistory();
  if (NULL == history || !history->OpenDatabase(file_name, read_write)) {
    delete history;
    return NULL;
  }

  LogCvmfs(kLogHistory, kLogDebug,
           "opened history database '%s' for repository '%s' %s",
           file_name.c_str(), history->fqrn().c_str(),
           ((history->IsWritable()) ? "(writable)" : ""));

  return history;
}


SqliteHistory* SqliteHistory::Create(const std::string &file_name,
                                     const std::string &fqrn) {
  SqliteHistory *history = new SqliteHistory();
  if (NULL == history || !history->CreateDatabase(file_name, fqrn)) {
    delete history;
    return NULL;
  }

  LogCvmfs(kLogHistory, kLogDebug, "created empty history database '%s' for"
                                   "repository '%s'",
           file_name.c_str(), fqrn.c_str());
  return history;
}


bool SqliteHistory::OpenDatabase(
  const std::string &file_name,
  const bool read_write
) {
  assert(!database_.IsValid());
  const HistoryDatabase::OpenMode mode = (read_write)
                                           ? HistoryDatabase::kOpenReadWrite
                                           : HistoryDatabase::kOpenReadOnly;
  database_ = HistoryDatabase::Open(file_name, mode);
  if (!database_.IsValid()) {
    return false;
  }

  if (!database_->HasProperty(HistoryDatabase::kFqrnKey)) {
    LogCvmfs(kLogHistory, kLogDebug, "opened history database does not provide "
                                     "an FQRN under '%s'",
             HistoryDatabase::kFqrnKey.c_str());
    return false;
  }

  set_fqrn(database_->GetProperty<std::string>(HistoryDatabase::kFqrnKey));
  PrepareQueries();
  return true;
}


bool SqliteHistory::CreateDatabase(const std::string &file_name,
                                   const std::string &repo_name) {
  assert(!database_.IsValid());
  assert(fqrn().empty());
  set_fqrn(repo_name);
  database_ = HistoryDatabase::Create(file_name);
  if (!database_.IsValid() || !database_->InsertInitialValues(repo_name)) {
    LogCvmfs(kLogHistory, kLogDebug,
             "failed to initialize empty database '%s', for repository '%s'",
             file_name.c_str(), repo_name.c_str());
    return false;
  }

  PrepareQueries();
  return true;
}


void SqliteHistory::PrepareQueries() {
  assert(database_.IsValid());

  find_tag_           = new SqlFindTag          (database_.weak_ref());
  find_tag_by_date_   = new SqlFindTagByDate    (database_.weak_ref());
  count_tags_         = new SqlCountTags        (database_.weak_ref());
  list_tags_          = new SqlListTags         (database_.weak_ref());
  get_hashes_         = new SqlGetHashes        (database_.weak_ref());
  list_rollback_tags_ = new SqlListRollbackTags (database_.weak_ref());
  list_branches_      = new SqlListBranches     (database_.weak_ref());

  if (database_->ContainsRecycleBin()) {
    recycle_list_ = new SqlRecycleBinList(database_.weak_ref());
  }

  if (IsWritable()) {
    insert_tag_         = new SqlInsertTag          (database_.weak_ref());
    remove_tag_         = new SqlRemoveTag          (database_.weak_ref());
    rollback_tag_       = new SqlRollbackTag        (database_.weak_ref());
    recycle_empty_      = new SqlRecycleBinFlush    (database_.weak_ref());
    insert_branch_      = new SqlInsertBranch       (database_.weak_ref());
    find_branch_head_   = new SqlFindBranchHead     (database_.weak_ref());
  }
}


bool SqliteHistory::BeginTransaction()  const {
  return database_->BeginTransaction();
}


bool SqliteHistory::CommitTransaction() const {
  return database_->CommitTransaction();
}


bool SqliteHistory::SetPreviousRevision(const shash::Any &history_hash) {
  assert(database_.IsValid());
  assert(IsWritable());
  return database_->SetProperty(kPreviousRevisionKey, history_hash.ToString());
}


shash::Any SqliteHistory::previous_revision() const {
  assert(database_.IsValid());
  const std::string hash_str =
    database_->GetProperty<std::string>(kPreviousRevisionKey);
  return shash::MkFromHexPtr(shash::HexPtr(hash_str), shash::kSuffixHistory);
}


bool SqliteHistory::IsWritable() const {
  assert(database_.IsValid());
  return database_->read_write();
}

unsigned SqliteHistory::GetNumberOfTags() const {
  assert(database_.IsValid());
  assert(count_tags_.IsValid());
  bool retval = count_tags_->FetchRow();
  assert(retval);
  const unsigned count = count_tags_->RetrieveCount();
  retval = count_tags_->Reset();
  assert(retval);
  return count;
}


bool SqliteHistory::Insert(const History::Tag &tag) {
  assert(database_.IsValid());
  assert(insert_tag_.IsValid());

  return insert_tag_->BindTag(tag) &&
         insert_tag_->Execute()    &&
         insert_tag_->Reset();
}


bool SqliteHistory::Remove(const std::string &name) {
  assert(database_.IsValid());
  assert(remove_tag_.IsValid());

  Tag condemned_tag;
  if (!GetByName(name, &condemned_tag)) {
    return true;
  }

  return remove_tag_->BindName(name)      &&
         remove_tag_->Execute()           &&
         remove_tag_->Reset();
}


bool SqliteHistory::Exists(const std::string &name) const {
  Tag existing_tag;
  return GetByName(name, &existing_tag);
}


bool SqliteHistory::GetByName(const std::string &name, Tag *tag) const {
  assert(database_.IsValid());
  assert(find_tag_.IsValid());
  assert(NULL != tag);

  if (!find_tag_->BindName(name) || !find_tag_->FetchRow()) {
    find_tag_->Reset();
    return false;
  }

  *tag = find_tag_->RetrieveTag();
  return find_tag_->Reset();
}


bool SqliteHistory::GetByDate(const time_t timestamp, Tag *tag) const {
  assert(database_.IsValid());
  assert(find_tag_by_date_.IsValid());
  assert(NULL != tag);

  if (!find_tag_by_date_->BindTimestamp(timestamp) ||
      !find_tag_by_date_->FetchRow())
  {
    find_tag_by_date_->Reset();
    return false;
  }

  *tag = find_tag_by_date_->RetrieveTag();
  return find_tag_by_date_->Reset();
}


bool SqliteHistory::List(std::vector<Tag> *tags) const {
  assert(list_tags_.IsValid());
  return RunListing(tags, list_tags_.weak_ref());
}


template <class SqlListingT>
bool SqliteHistory::RunListing(std::vector<Tag> *list, SqlListingT *sql) const {
  assert(database_.IsValid());
  assert(NULL != list);

  while (sql->FetchRow()) {
    list->push_back(sql->RetrieveTag());
  }

  return sql->Reset();
}


bool SqliteHistory::GetBranchHead(const string &branch_name, Tag *tag) const {
  assert(database_.IsValid());
  assert(find_branch_head_.IsValid());
  assert(tag != NULL);

  if (!find_branch_head_->BindBranchName(branch_name) ||
      !find_branch_head_->FetchRow())
  {
    find_branch_head_->Reset();
    return false;
  }

  *tag = find_branch_head_->RetrieveTag();
  return find_branch_head_->Reset();
}


bool SqliteHistory::ExistsBranch(const string &branch_name) const {
  vector<Branch> branches;
  if (!ListBranches(&branches))
    return false;
  for (unsigned i = 0; i < branches.size(); ++i) {
    if (branches[i].branch == branch_name)
      return true;
  }
  return false;
}


bool SqliteHistory::InsertBranch(const Branch &branch) {
  assert(database_.IsValid());
  assert(insert_branch_.IsValid());

  return insert_branch_->BindBranch(branch) &&
         insert_branch_->Execute()    &&
         insert_branch_->Reset();
}


bool SqliteHistory::PruneBranches() {
  // Parent pointers might point to abandoned branches.  Redirect them to the
  // parent of the abandoned branch.  This has to be repeated until the fix
  // point is reached.  It always works because we never delete the root branch
  sqlite::Sql sql_fix_parent_pointers(database_->sqlite_db(),
    "INSERT OR REPLACE INTO branches (branch, parent, initial_revision) "
    "SELECT branches.branch, abandoned_parent, branches.initial_revision "
    "  FROM branches "
    "  INNER JOIN (SELECT DISTINCT branches.branch AS abandoned_branch, "
    "              branches.parent AS abandoned_parent FROM branches "
    "              LEFT OUTER JOIN tags ON (branches.branch=tags.branch)"
    "              WHERE tags.branch IS NULL) "
    "  ON (branches.parent=abandoned_branch);");
  // Detect if fix point is reached
  sqlite::Sql sql_remaining_rows(database_->sqlite_db(),
    "SELECT count(*) FROM branches "
    "INNER JOIN "
    "  (SELECT DISTINCT branches.branch AS abandoned_branch FROM branches "
    "   LEFT OUTER JOIN tags ON (branches.branch=tags.branch) "
    "   WHERE tags.branch IS NULL) "
    "ON (branches.parent=abandoned_branch);");

  bool retval;
  do {
    retval = sql_remaining_rows.FetchRow();
    if (!retval)
      return false;
    int64_t count = sql_remaining_rows.RetrieveInt64(0);
    assert(count >= 0);
    if (count == 0)
      break;
    retval = sql_remaining_rows.Reset();
    assert(retval);

    retval = sql_fix_parent_pointers.Execute();
    if (!retval)
      return false;
    retval = sql_fix_parent_pointers.Reset();
    assert(retval);
  } while (true);

  sqlite::Sql sql_remove_branches(database_->sqlite_db(),
    "DELETE FROM branches "
    "WHERE branch NOT IN (SELECT DISTINCT branch FROM tags);");
  retval = sql_remove_branches.Execute();
  return retval;
}


bool SqliteHistory::ListBranches(vector<Branch> *branches) const {
  while (list_branches_->FetchRow()) {
    branches->push_back(list_branches_->RetrieveBranch());
  }

  return list_branches_->Reset();
}


bool SqliteHistory::ListRecycleBin(std::vector<shash::Any> *hashes) const {
  assert(database_.IsValid());

  if (!database_->ContainsRecycleBin()) {
    return false;
  }

  assert(NULL != hashes);
  hashes->clear();
  while (recycle_list_->FetchRow()) {
    hashes->push_back(recycle_list_->RetrieveHash());
  }

  return recycle_list_->Reset();
}


bool SqliteHistory::EmptyRecycleBin() {
  assert(database_.IsValid());
  assert(IsWritable());
  assert(recycle_empty_.IsValid());
  return recycle_empty_->Execute() &&
         recycle_empty_->Reset();
}


bool SqliteHistory::Rollback(const Tag &updated_target_tag) {
  assert(database_.IsValid());
  assert(IsWritable());
  assert(rollback_tag_.IsValid());

  Tag old_target_tag;
  bool success = false;

  // open a transaction (if non open yet)
  const bool need_to_commit = BeginTransaction();

  // retrieve the old version of the target tag from the history
  success = GetByName(updated_target_tag.name, &old_target_tag);
  if (!success) {
    LogCvmfs(kLogHistory, kLogDebug, "failed to retrieve old target tag '%s'",
                                     updated_target_tag.name.c_str());
    return false;
  }

  // sanity checks
  assert(old_target_tag.description == updated_target_tag.description);

  // rollback the history to the target tag
  // (essentially removing all intermediate tags + the old target tag)
  success = rollback_tag_->BindTargetTag(old_target_tag) &&
            rollback_tag_->Execute()                     &&
            rollback_tag_->Reset();
  if (!success || Exists(old_target_tag.name)) {
    LogCvmfs(kLogHistory, kLogDebug, "failed to remove intermediate tags "
                                     "until '%s' - '%d'",
                                     old_target_tag.name.c_str(),
                                     old_target_tag.revision);
    return false;
  }

  // insert the provided updated target tag into the history concluding the
  // rollback operation
  success = Insert(updated_target_tag);
  if (!success) {
    LogCvmfs(kLogHistory, kLogDebug, "failed to insert updated target tag '%s'",
                                     updated_target_tag.name.c_str());
    return false;
  }

  if (need_to_commit) {
    success = CommitTransaction();
    assert(success);
  }

  return true;
}


bool SqliteHistory::ListTagsAffectedByRollback(
                                            const std::string  &target_tag_name,
                                            std::vector<Tag>   *tags) const {
  // retrieve the old version of the target tag from the history
  Tag target_tag;
  if (!GetByName(target_tag_name, &target_tag)) {
    LogCvmfs(kLogHistory, kLogDebug, "failed to retrieve target tag '%s'",
                                     target_tag_name.c_str());
    return false;
  }

  // prepage listing command to find affected tags for a potential rollback
  if (!list_rollback_tags_->BindTargetTag(target_tag)) {
    LogCvmfs(kLogHistory, kLogDebug,
             "failed to prepare rollback listing query");
    return false;
  }

  // run the listing and return the results
  return RunListing(tags, list_rollback_tags_.weak_ref());
}


bool SqliteHistory::GetHashes(std::vector<shash::Any> *hashes) const {
  assert(database_.IsValid());
  assert(NULL != hashes);

  while (get_hashes_->FetchRow()) {
    hashes->push_back(get_hashes_->RetrieveHash());
  }

  return get_hashes_->Reset();
}


void SqliteHistory::TakeDatabaseFileOwnership() {
  assert(database_.IsValid());
  database_->TakeFileOwnership();
}


void SqliteHistory::DropDatabaseFileOwnership() {
  assert(database_.IsValid());
  database_->DropFileOwnership();
}

}  // namespace history
