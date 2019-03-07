// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "olap/snapshot_manager.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <algorithm>
#include <iterator>
#include <map>
#include <set>

#include <boost/date_time/gregorian/gregorian.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/filesystem.hpp>

#include "olap/olap_snapshot_converter.h"

using boost::filesystem::canonical;
using boost::filesystem::copy_file;
using boost::filesystem::copy_option;
using boost::filesystem::path;
using std::map;
using std::nothrow;
using std::set;
using std::string;
using std::stringstream;
using std::vector;
using std::list;

namespace doris {

SnapshotManager* SnapshotManager::_s_instance = nullptr;
std::mutex SnapshotManager::_mlock;

SnapshotManager* SnapshotManager::instance() {
    if (_s_instance == nullptr) {
        std::lock_guard<std::mutex> lock(_mlock);
        if (_s_instance == nullptr) {
            _s_instance = new SnapshotManager();
        }
    }
    return _s_instance;
}

OLAPStatus SnapshotManager::make_snapshot(
        const TSnapshotRequest& request,
        string* snapshot_path) {
    OLAPStatus res = OLAP_SUCCESS;
    if (snapshot_path == nullptr) {
        LOG(WARNING) << "output parameter cannot be NULL";
        return OLAP_ERR_INPUT_PARAMETER_ERROR;
    }

    TabletSharedPtr ref_tablet = TabletManager::instance()->get_tablet(request.tablet_id, request.schema_hash);
    if (ref_tablet.get() == nullptr) {
        LOG(WARNING) << "failed to get tablet. tablet=" << request.tablet_id
                  << " schema_hash=" << request.schema_hash;
        return OLAP_ERR_TABLE_NOT_FOUND;
    }

    res = _create_snapshot_files(ref_tablet, request, snapshot_path, request.preferred_snapshot_version);
    // if all nodes has been upgraded, it can be removed
    if (request.__isset.missing_version && res == OLAP_SUCCESS) {
        (const_cast<TSnapshotRequest&>(request)).__set_allow_incremental_clone(true);
    }

    if (res != OLAP_SUCCESS) {
        LOG(WARNING) << "failed to make snapshot. res=" << res
                     << " tablet=" << request.tablet_id
                     << " schema_hash=" << request.schema_hash;
        return res;
    }

    LOG(INFO) << "success to make snapshot. path=['" << snapshot_path << "']";
    return res;
}

OLAPStatus SnapshotManager::release_snapshot(const string& snapshot_path) {
    // 如果请求的snapshot_path位于root/snapshot文件夹下，则认为是合法的，可以删除
    // 否则认为是非法请求，返回错误结果
    auto stores = StorageEngine::instance()->get_stores();
    for (auto store : stores) {
        path boost_root_path(store->path());
        string abs_path = canonical(boost_root_path).string();

        if (snapshot_path.compare(0, abs_path.size(), abs_path) == 0
                && snapshot_path.compare(abs_path.size(),
                        SNAPSHOT_PREFIX.size(), SNAPSHOT_PREFIX) == 0) {
            remove_all_dir(snapshot_path);
            VLOG(3) << "success to release snapshot path. [path='" << snapshot_path << "']";

            return OLAP_SUCCESS;
        }
    }

    LOG(WARNING) << "released snapshot path illegal. [path='" << snapshot_path << "']";
    return OLAP_ERR_CE_CMD_PARAMS_ERROR;
}

OLAPStatus SnapshotManager::_calc_snapshot_id_path(
        const TabletSharedPtr& tablet,
        string* out_path) {
    OLAPStatus res = OLAP_SUCCESS;
    if (out_path == nullptr) {
        LOG(WARNING) << "output parameter cannot be NULL";
        return OLAP_ERR_INPUT_PARAMETER_ERROR;
    }

    // get current timestamp string
    string time_str;
    if ((res = gen_timestamp_string(&time_str)) != OLAP_SUCCESS) {
        LOG(WARNING) << "failed to generate time_string when move file to trash."
                     << "err code=" << res;
        return res;
    }

    stringstream snapshot_id_path_stream;
    MutexLock auto_lock(&_snapshot_mutex); // will automatically unlock when function return.
    snapshot_id_path_stream << tablet->data_dir()->path() << SNAPSHOT_PREFIX
                            << "/" << time_str << "." << _snapshot_base_id++;
    *out_path = snapshot_id_path_stream.str();
    return res;
}

string SnapshotManager::get_schema_hash_full_path(
        const TabletSharedPtr& ref_tablet,
        const string& location) const {
    stringstream schema_full_path_stream;
    schema_full_path_stream << location
                            << "/" << ref_tablet->tablet_id()
                            << "/" << ref_tablet->schema_hash();
    string schema_full_path = schema_full_path_stream.str();

    return schema_full_path;
}

string SnapshotManager::_get_header_full_path(
        const TabletSharedPtr& ref_tablet,
        const std::string& schema_hash_path) const {
    stringstream header_name_stream;
    header_name_stream << schema_hash_path << "/" << ref_tablet->tablet_id() << ".hdr";
    return header_name_stream.str();
}

void SnapshotManager::update_header_file_info(
        const vector<RowsetSharedPtr>& consistent_rowsets,
        TabletMeta* tablet_meta) {
    vector<RowsetMetaSharedPtr> rs_metas;
    for (auto& rs : consistent_rowsets) {
        rs_metas.push_back(rs->rowset_meta());
    }
    tablet_meta->revise_rs_metas(rs_metas);
}

OLAPStatus SnapshotManager::_link_index_and_data_files(
        const string& schema_hash_path,
        const TabletSharedPtr& ref_tablet,
        const std::vector<RowsetSharedPtr>& consistent_rowsets) {
    OLAPStatus res = OLAP_SUCCESS;
    for (auto& rs : consistent_rowsets) {
        std::vector<std::string> success_files;
        RETURN_NOT_OK(rs->make_snapshot(schema_hash_path, &success_files));
    }
    return res;
}

OLAPStatus SnapshotManager::_create_snapshot_files(
        const TabletSharedPtr& ref_tablet,
        const TSnapshotRequest& request,
        string* snapshot_path, 
        int32_t snapshot_version) {
    OLAPStatus res = OLAP_SUCCESS;
    if (snapshot_path == nullptr) {
        LOG(WARNING) << "output parameter cannot be NULL";
        return OLAP_ERR_INPUT_PARAMETER_ERROR;
    }

    string snapshot_id_path;
    res = _calc_snapshot_id_path(ref_tablet, &snapshot_id_path);
    if (res != OLAP_SUCCESS) {
        LOG(WARNING) << "failed to calc snapshot_id_path, ref tablet="
                     << ref_tablet->data_dir()->path();
        return res;
    }

    string schema_full_path = get_schema_hash_full_path(
            ref_tablet, snapshot_id_path);
    string header_path = _get_header_full_path(ref_tablet, schema_full_path);
    if (check_dir_existed(schema_full_path)) {
        VLOG(10) << "remove the old schema_full_path.";
        remove_all_dir(schema_full_path);
    }
    create_dirs(schema_full_path);
    path boost_path(snapshot_id_path);
    string snapshot_id = canonical(boost_path).string();
    do {
        DataDir* data_dir = ref_tablet->data_dir();
        std::unique_ptr<TabletMeta> new_tablet_meta(new(nothrow) TabletMeta(data_dir));
        if (new_tablet_meta == nullptr) {
            LOG(WARNING) << "fail to malloc TabletMeta.";
            res = OLAP_ERR_MALLOC_ERROR;
            break;
        }
        vector<RowsetSharedPtr> consistent_rowsets;
        if (request.__isset.missing_version) {
            ReadLock rdlock(ref_tablet->get_header_lock_ptr());
            for (int64_t missed_version : request.missing_version) {
                Version version = { missed_version, missed_version };
                const RowsetSharedPtr rowset = ref_tablet->get_rowset_by_version(version);
                if (rowset != nullptr) {
                    consistent_rowsets.push_back(rowset);
                } else {
                    LOG(WARNING) << "failed to find missed version when snapshot. "
                                 << " tablet=" << request.tablet_id
                                 << " schema_hash=" << request.schema_hash
                                 << " version=" << version.first << "-" << version.second;
                    res = OLAP_ERR_VERSION_NOT_EXIST;
                    break;
                }
            }
            res = TabletMetaManager::get_header(data_dir, ref_tablet->tablet_id(), ref_tablet->schema_hash(), 
                new_tablet_meta.get());
            if (res != OLAP_SUCCESS) {
                LOG(WARNING) << "fail to load header. res=" << res
                             << " tablet_id=" << ref_tablet->tablet_id() 
                             << " schema_hash=" << ref_tablet->schema_hash();
                break;
            }
        } else {
            ReadLock rdlock(ref_tablet->get_header_lock_ptr());
            // get latest version
            const RowsetSharedPtr lastest_version = ref_tablet->rowset_with_max_version();
            if (lastest_version == nullptr) {
                LOG(WARNING) << "tablet has not any version. path="
                             << ref_tablet->full_name().c_str();
                res = OLAP_ERR_VERSION_NOT_EXIST;
                break;
            }
            // get snapshot version, use request.version if specified
            int32_t version = lastest_version->end_version();
            if (request.__isset.version) {
                if (lastest_version->end_version() < request.version
                        || (lastest_version->start_version() == lastest_version->end_version()
                        && lastest_version->end_version() == request.version
                        && lastest_version->version_hash() != request.version_hash)) {
                    LOG(WARNING) << "invalid make snapshot request. "
                                 << " version=" << lastest_version->end_version()
                                 << " version_hash=" << lastest_version->version_hash()
                                 << " req_version=" << request.version
                                 << " req_version_hash=" << request.version_hash;
                    res = OLAP_ERR_INPUT_PARAMETER_ERROR;
                    break;
                }
                version = request.version;
            }
            // get shortest version path
            res = ref_tablet->capture_consistent_rowsets(Version(0, version), &consistent_rowsets);
            if (res != OLAP_SUCCESS) {
                LOG(WARNING) << "fail to select versions to span. res=" << res;
                break;
            }

            res = TabletMetaManager::get_header(data_dir, ref_tablet->tablet_id(), ref_tablet->schema_hash(), 
                new_tablet_meta.get());
            if (res != OLAP_SUCCESS) {
                LOG(WARNING) << "fail to load header. res=" << res
                             << " tablet_id=" << ref_tablet->tablet_id() 
                             << " schema_hash=" << ref_tablet->schema_hash();
                break;
            }
        }

        vector<RowsetMetaSharedPtr> rs_metas;
        for (auto& rs : consistent_rowsets) {
            std::vector<std::string> success_files;
            res = rs->make_snapshot(schema_full_path, &success_files);
            if (res != OLAP_SUCCESS) { break; }
            rs_metas.push_back(rs->rowset_meta());
        }
        if (res != OLAP_SUCCESS) {
            LOG(WARNING) << "fail to create hard link. [path=" << snapshot_id_path << "]";
            break;
        }

        if (request.__isset.missing_version) {
            new_tablet_meta->revise_inc_rs_metas(rs_metas);
        } else {
            new_tablet_meta->revise_rs_metas(rs_metas);
        }
        if (snapshot_version < PREFERRED_SNAPSHOT_VERSION) {
            OlapSnapshotConverter converter;
            TabletMetaPB tablet_meta_pb;
            OLAPHeaderMessage olap_header_msg;
            new_tablet_meta->to_meta_pb(&tablet_meta_pb);
            converter.to_old_snapshot(tablet_meta_pb, schema_full_path, schema_full_path, &olap_header_msg);
            // save new header to snapshot header path
            res = converter.save(header_path, olap_header_msg);
        } else {
            res = new_tablet_meta->save(header_path);
        }
        if (res != OLAP_SUCCESS) {
            break;
        }
        
        // append a single delta if request.version is end_version of cumulative delta
        if (request.__isset.version) {
            for (auto& rs : consistent_rowsets) {
                if (rs->end_version() == request.version) {
                    if (rs->start_version() != request.version) {
                        // visible version in fe is 900
                        // A need to clone 900 from B, but B's last version is 901, and 901 is not a visible version
                        // and 901 will be reverted
                        // since 900 is not the last version in B, 900 maybe compacted with other versions
                        // if A only get 900, then A's last version will be a comulative delta
                        // many codes in be assumes that the last version is a single delta
                        // both clone and backup restore depend on this logic
                        // TODO (yiguolei) fix it in the future
                        res = _append_single_delta(request, data_dir);
                        if (res != OLAP_SUCCESS) {
                            LOG(WARNING) << "fail to append single delta. res=" << res;
                        }
                    }
                    break;
                }
            }
        }
    } while (0);

    if (res != OLAP_SUCCESS) {
        LOG(WARNING) << "fail to make snapshot, try to delete the snapshot path. path="
                     << snapshot_id_path.c_str();

        if (check_dir_existed(snapshot_id_path)) {
            VLOG(3) << "remove snapshot path. [path=" << snapshot_id_path << "]";
            remove_all_dir(snapshot_id_path);
        }
    } else {
        *snapshot_path = snapshot_id;
    }

    return res;
}

OLAPStatus SnapshotManager::_append_single_delta(
        const TSnapshotRequest& request, DataDir* store) {
    OLAPStatus res = OLAP_SUCCESS;
    string root_path = store->path();
    TabletMeta* new_tablet_meta = new(nothrow) TabletMeta(store);
    if (new_tablet_meta == nullptr) {
        LOG(WARNING) << "fail to malloc TabletMeta.";
        return OLAP_ERR_MALLOC_ERROR;
    }

    res = TabletMetaManager::get_header(store, request.tablet_id, request.schema_hash, new_tablet_meta);
    if (res != OLAP_SUCCESS) {
        LOG(WARNING) << "fail to create tablet from header file. " 
                     << " tablet_id=" << request.tablet_id
                     << " schema_hash=" << request.schema_hash;
        return res;
    }
    auto tablet = Tablet::create_tablet_from_meta(new_tablet_meta, store);
    if (tablet == nullptr) {
        LOG(WARNING) << "fail to load tablet. " 
                     << " res=" << res
                     << " tablet_id=" << request.tablet_id
                     << " schema_hash=" << request.schema_hash;
        return OLAP_ERR_INPUT_PARAMETER_ERROR;
    }

    res = tablet->init();
    if (res != OLAP_SUCCESS) {
        LOG(WARNING) << "fail to load tablet. [res=" << res 
                     << " header_path=" << store->path();
        return res;
    }

    const RowsetSharedPtr lastest_version = tablet->rowset_with_max_version();
    if (lastest_version->start_version() != request.version) {
        TPushReq empty_push;
        empty_push.tablet_id = request.tablet_id;
        empty_push.schema_hash = request.schema_hash;
        empty_push.version = request.version + 1;
        empty_push.version_hash = 0;
        
        PushHandler handler;
        // res = handler.process(tablet, empty_push, PUSH_NORMAL, NULL);
        // TODO (yiguolei) should create a empty version, call create new rowset meta and set version
        // just return success to skip push a empty rowset into the snapshot since has alreay removed
        // batch process code from push handler
        res = OLAP_SUCCESS;
        if (res != OLAP_SUCCESS) {
            LOG(WARNING) << "fail to push empty version. " 
                         << " res=" << res 
                         << " version=" << empty_push.version;
            return res;
        }
    }
    return res;
}

}  // namespace doris
