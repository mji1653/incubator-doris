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

#include <string>
#include <sstream>
#include "es_scan_reader.h"
#include "es_scroll_query.h"
#include "common/logging.h"
#include "common/status.h"
#include <map>

namespace doris {
const std::string REUQEST_SCROLL_FILTER_PATH = "filter_path=_scroll_id,hits.hits._source,hits.total,_id,hits.hits._source.fields";
const std::string REQUEST_SCROLL_PATH = "_scroll";
const std::string REQUEST_PREFERENCE_PREFIX = "&preference=shards:";
const std::string REQUEST_SEARCH_SCROLL_PATH = "/_search/scroll";
const std::string REQUEST_SEPARATOR = "/";
const std::string REQUEST_SCROLL_TIME = "5m";

ESScanReader::ESScanReader(const std::string& target, uint16_t size, const std::map<std::string, std::string>& props) {
    LOG(INFO) << "ESScanReader ";
    _target = target;
    _batch_size = size;
    _index = props.at(KEY_INDEX);
    _type = props.at(KEY_TYPE);
    if (props.find(KEY_USER_NAME) != props.end()) {
        _user_name = props.at(KEY_USER_NAME);
    }
    if (props.find(KEY_PASS_WORD) != props.end()){
        _passwd = props.at(KEY_PASS_WORD);
    }
    if (props.find(KEY_SHARDS) != props.end()) {
        _shards = props.at(KEY_SHARDS);
    }
    if (props.find(KEY_QUERY) != props.end()) {
        _query = props.at(KEY_QUERY);
    }
    _init_scroll_url = _target + REQUEST_SEPARATOR + _index + REQUEST_SEPARATOR + _type + "/_search?scroll=" + REQUEST_SCROLL_TIME + REQUEST_PREFERENCE_PREFIX + _shards + "&" + REUQEST_SCROLL_FILTER_PATH;
    _next_scroll_url = _target + REQUEST_SEARCH_SCROLL_PATH + "?" + REUQEST_SCROLL_FILTER_PATH;
    _eos = false;
    _parser.set_batch_size(size);
}

ESScanReader::~ESScanReader() {
}

Status ESScanReader::open() {
    _is_first = true;
    RETURN_IF_ERROR(_network_client.init(_init_scroll_url));
    _network_client.set_basic_auth(_user_name, _passwd);
    _network_client.set_content_type("application/json");
    // phase open, we cached the first response for `get_next` phase
    _network_client.execute_post_request(_query, &_cached_response);
    long status = _network_client.get_http_status();
    if (status != 200) {
        LOG(WARNING) << "invalid response http status for open: " << status;
        return Status(_cached_response);
    }
    VLOG(1) << "open _cached response: " << _cached_response;
    RETURN_IF_ERROR(_parser.parse(_cached_response));
    _eos = _parser.has_next();
    return Status::OK;
}

Status ESScanReader::get_next(bool* eos, std::string* response) {
    // if is first scroll request, should return the cached response
    if (_is_first) {
        // maybe the index or shard is empty
        if (_eos) {
            *eos = true;
            return Status::OK;
        }
        _is_first = false;
        *eos = _eos;
        *response = _cached_response;
        return Status::OK;
    }
    RETURN_IF_ERROR(_network_client.init(_next_scroll_url));
    _network_client.set_basic_auth(_user_name, _passwd);
    _network_client.set_content_type("application/json");
    _network_client.set_timeout_ms(5 * 1000);
    RETURN_IF_ERROR(_network_client.execute_post_request(ESScrollQueryBuilder::build_next_scroll_body(_scroll_id, REQUEST_SCROLL_TIME), response));
    long status = _network_client.get_http_status();
    if (status == 404) {
        LOG(WARNING) << "request scroll search failure 404[" 
                     << ", response: " << (response->empty() ? "empty response" : *response);
        return Status("No search context found for " + _scroll_id);
    }
    if (status != 200) {
        LOG(WARNING) << "request scroll search failure[" 
                     << "http status" << status
                     << ", response: " << (response->empty() ? "empty response" : *response);
        if (status == 404) {
                return Status("No search context found for " + _scroll_id);
            }
        return Status("request scroll search failure: " + (response->empty() ? "empty response" : *response));        
    }
    RETURN_IF_ERROR(_parser.parse(*response));
    *eos = _eos = _parser.has_next();
    return Status::OK;
}

Status ESScanReader::close() {
    std::string scratch_target = _target + REQUEST_SEARCH_SCROLL_PATH;
    RETURN_IF_ERROR(_network_client.init(scratch_target));
    _network_client.set_basic_auth(_user_name, _passwd);
    _network_client.set_method(DELETE);
    _network_client.set_content_type("application/json");
    _network_client.set_timeout_ms(5 * 1000);
    std::string response;
    RETURN_IF_ERROR(_network_client.execute_delete_request(ESScrollQueryBuilder::build_clear_scroll_body(_scroll_id), &response));
    if (_network_client.get_http_status() == 200) {
        return Status::OK;
    } else {
        return Status("es_scan_reader delete scroll context failure");
    }
}
}
