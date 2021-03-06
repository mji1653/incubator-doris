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

#pragma once

#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/status.h"
#include "common/object_pool.h"
#include "exec/data_sink.h"
#include "exec/olap_table_info.h"
#include "gen_cpp/Types_types.h"
#include "gen_cpp/internal_service.pb.h"
#include "gen_cpp/palo_internal_service.pb.h"
#include "util/bitmap.h"
#include "util/thrift_util.h"
#include "util/ref_count_closure.h"

namespace doris {

class Bitmap;
class MemTracker;
class RuntimeProfile;
class RowDescriptor;
class Tuple;
class TupleDescriptor;
class ExprContext;
class TExpr;

namespace stream_load {
 
class OlapTableSink;

class NodeChannel {
public:
    NodeChannel(OlapTableSink* parent, int64_t index_id, int64_t node_id, int32_t schema_hash);
    ~NodeChannel() noexcept;

    // called before open, used to add tablet loacted in this backend
    void add_tablet(const TTabletWithPartition& tablet) {
        _all_tablets.emplace_back(tablet);
    }

    Status init(RuntimeState* state);

    // we use open/open_wait to parallel
    void open();
    Status open_wait();

    Status add_row(Tuple* tuple, int64_t tablet_id);

    Status close(RuntimeState* state);
    Status close_wait(RuntimeState* state);

    void cancel();

    int64_t node_id() const { return _node_id; }

    void set_failed() { _already_failed = true; }
    bool already_failed() const { return _already_failed; }
    const NodeInfo* node_info() const { return _node_info; }

private:
    Status _send_cur_batch(bool eos = false);
    // wait inflight packet finish, return error if inflight packet return failed
    Status _wait_in_flight_packet();

    Status _close(RuntimeState* state);

private:
    OlapTableSink* _parent = nullptr;
    int64_t _index_id = -1;
    int64_t _node_id = -1;
    int32_t _schema_hash = 0;

    TupleDescriptor* _tuple_desc = nullptr;
    const NodeInfo* _node_info = nullptr;

    bool _already_failed = false;
    bool _has_in_flight_packet = false;
    int _rpc_timeout_ms = 50000;
    int64_t _next_packet_seq = 0;

    std::unique_ptr<RowBatch> _batch;
    palo::PInternalService_Stub* _stub = nullptr;
    RefCountClosure<PTabletWriterOpenResult>* _open_closure = nullptr;
    RefCountClosure<PTabletWriterAddBatchResult>* _add_batch_closure = nullptr;

    std::vector<TTabletWithPartition> _all_tablets;
    PTabletWriterAddBatchRequest _add_batch_request;
};

class IndexChannel {
public:
    IndexChannel(OlapTableSink* parent, int64_t index_id, int32_t schema_hash)
            : _parent(parent), _index_id(index_id),
            _schema_hash(schema_hash) {
    }
    ~IndexChannel();

    Status init(RuntimeState* state,
                const std::vector<TTabletWithPartition>& tablets);
    Status open();
    Status add_row(Tuple* tuple, int64_t tablet_id);

    Status close(RuntimeState* state);

    void cancel();

private:
    // return true if this load can't success.
    bool _handle_failed_node(NodeChannel* channel);

private:
    OlapTableSink* _parent;
    int64_t _index_id;
    int32_t _schema_hash;
    int _num_failed_channels = 0;

    // BeId -> channel
    std::unordered_map<int64_t, NodeChannel*> _node_channels;
    // from tablet_id to backend channel
    std::unordered_map<int64_t, std::vector<NodeChannel*>> _channels_by_tablet;
};

// write data to Olap Table.
// this class distributed data according to 
class OlapTableSink : public DataSink {
public:
    // Construct from thrift struct which is generated by FE.
    OlapTableSink(ObjectPool* pool,
                  const RowDescriptor& row_desc,
                  const std::vector<TExpr>& texprs,
                  Status* status);
    ~OlapTableSink() override;

    Status init(const TDataSink& sink) override;

    Status prepare(RuntimeState* state) override;

    Status open(RuntimeState* state) override;

    Status send(RuntimeState* state, RowBatch* batch) override;

    Status close(RuntimeState* state, Status close_status) override;

    // Returns the runtime profile for the sink.
    RuntimeProfile* profile() override {
        return _profile;
    }

private:
    // convert input batch to output batch which will be loaded into OLAP table.
    // this is only used in insert statement.
    void _convert_batch(RuntimeState* state, RowBatch* input_batch, RowBatch* output_batch);

    // make input data valid for OLAP table
    // return number of invalid/filtered rows.
    // invalid row number is set in Bitmap
    int _validate_data(RuntimeState* state, RowBatch* batch, Bitmap* filter_bitmap);

private:
    friend class NodeChannel;
    friend class IndexChannel;

    ObjectPool* _pool;
    const RowDescriptor& _input_row_desc;

    // unique load id
    PUniqueId _load_id;
    int64_t _txn_id = -1;
    int64_t _db_id = -1;
    int64_t _table_id = -1;
    int _num_repicas = -1;
    bool _need_gen_rollup = true;
    std::string _db_name;
    std::string _table_name;
    int _tuple_desc_id = -1;

    // this is tuple descriptor of destination OLAP table
    TupleDescriptor* _output_tuple_desc = nullptr;
    RowDescriptor* _output_row_desc = nullptr;
    std::vector<ExprContext*> _output_expr_ctxs;
    std::unique_ptr<RowBatch> _output_batch;

    bool _need_validate_data = false;

    // number of senders used to insert into OlapTable, if we only support single node insert,
    // all data from select should collectted and then send to OlapTable.
    // To support multiple senders, we maintain a channel for each sender.
    int _sender_id = -1;
    int _num_senders = -1;

    // TODO(zc): think about cache this data
    std::shared_ptr<OlapTableSchemaParam> _schema;
    OlapTablePartitionParam* _partition = nullptr;
    OlapTableLocationParam* _location = nullptr;
    DorisNodesInfo* _nodes_info = nullptr;

    RuntimeProfile* _profile = nullptr;
    MemTracker* _mem_tracker = nullptr;

    std::set<int64_t> _partition_ids;

    Bitmap _filter_bitmap;

    // index_channel
    std::vector<IndexChannel*> _channels;

    std::vector<DecimalValue> _max_decimal_val;
    std::vector<DecimalValue> _min_decimal_val;

    std::vector<DecimalV2Value> _max_decimalv2_val;
    std::vector<DecimalV2Value> _min_decimalv2_val;

    // Stats for this
    int64_t _convert_batch_ns = 0;
    int64_t _validate_data_ns = 0;
    int64_t _send_data_ns = 0;
    int64_t _number_input_rows = 0;
    int64_t _number_output_rows = 0;
    int64_t _number_filtered_rows = 0;

    RuntimeProfile::Counter* _input_rows_counter = nullptr;
    RuntimeProfile::Counter* _output_rows_counter = nullptr;
    RuntimeProfile::Counter* _filtered_rows_counter = nullptr;
    RuntimeProfile::Counter* _send_data_timer = nullptr;
    RuntimeProfile::Counter* _convert_batch_timer = nullptr;
    RuntimeProfile::Counter* _validate_data_timer = nullptr;
    RuntimeProfile::Counter* _open_timer = nullptr;
    RuntimeProfile::Counter* _close_timer = nullptr;
};

}
}
