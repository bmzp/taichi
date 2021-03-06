#include "taichi/codegen/codegen_metal.h"

#include <functional>
#include <string>

#include "taichi/ir/ir.h"
#include "taichi/util/line_appender.h"

TLANG_NAMESPACE_BEGIN
namespace metal {
namespace {

namespace shaders {
#include "taichi/platform/metal/shaders/runtime_structs.metal.h"
}  // namespace shaders

using BuffersEnum = MetalKernelAttributes::Buffers;

constexpr char kKernelThreadIdName[] = "utid_";  // 'u' for unsigned
constexpr char kRootBufferName[] = "root_addr";
constexpr char kGlobalTmpsBufferName[] = "global_tmps_addr";
constexpr char kArgsBufferName[] = "args_addr";
constexpr char kRuntimeBufferName[] = "runtime_addr";
constexpr char kArgsContextName[] = "args_ctx_";
constexpr char kRuntimeVarName[] = "runtime_";
constexpr char kListgenElemVarName[] = "listgen_elem_";

class MetalKernelCodegen : public IRVisitor {
 public:
  MetalKernelCodegen(const std::string &mtl_kernel_prefix,
                     const std::string &root_snode_type_name,
                     Kernel *kernel,
                     const StructCompiledResult *compiled_snode_structs)
      : mtl_kernel_prefix_(mtl_kernel_prefix),
        root_snode_type_name_(root_snode_type_name),
        kernel_(kernel),
        compiled_snodes_(compiled_snode_structs),
        needs_root_buffer_(compiled_snodes_->root_size > 0),
        args_attribs_(kernel_->args) {
    // allow_undefined_visitor = true;
  }

  const MetalKernelArgsAttributes &kernel_args_attribs() const {
    return args_attribs_;
  }

  const std::string &kernel_source_code() const {
    return line_appender_.lines();
  }

  const std::vector<MetalKernelAttributes> &kernels_attribs() const {
    return mtl_kernels_attribs_;
  }

  void run() {
    generate_mtl_header();
    generate_kernel_args_struct();
    kernel_->ir->accept(this);
  }

  void visit(Block *stmt) override {
    if (!is_top_level_) {
      line_appender_.push_indent();
    }
    for (auto &s : stmt->statements) {
      s->accept(this);
    }
    if (!is_top_level_) {
      line_appender_.pop_indent();
    }
  }

  void visit(AllocaStmt *alloca) override {
    emit(R"({} {}(0);)", metal_data_type_name(alloca->element_type()),
         alloca->raw_name());
  }

  void visit(ConstStmt *const_stmt) override {
    TI_ASSERT(const_stmt->width() == 1);
    emit("const {} {} = {};", metal_data_type_name(const_stmt->element_type()),
         const_stmt->raw_name(), const_stmt->val[0].stringify());
  }

  void visit(LocalLoadStmt *stmt) override {
    // TODO: optimize for partially vectorized load...
    bool linear_index = true;
    for (int i = 0; i < (int)stmt->ptr.size(); i++) {
      if (stmt->ptr[i].offset != i) {
        linear_index = false;
      }
    }
    if (stmt->same_source() && linear_index &&
        stmt->width() == stmt->ptr[0].var->width()) {
      auto ptr = stmt->ptr[0].var;
      emit("const {} {}({});", metal_data_type_name(stmt->element_type()),
           stmt->raw_name(), ptr->raw_name());
    } else {
      TI_NOT_IMPLEMENTED;
    }
  }

  void visit(LocalStoreStmt *stmt) override {
    emit(R"({} = {};)", stmt->ptr->raw_name(), stmt->data->raw_name());
  }

  void visit(GetRootStmt *stmt) override {
    // Should we assert |root_stmt_| is assigned only once?
    TI_ASSERT(needs_root_buffer_);
    root_stmt_ = stmt;
    emit(R"({} {}({});)", root_snode_type_name_, stmt->raw_name(),
         kRootBufferName);
  }

  void visit(GetChStmt *stmt) override {
    if (stmt->output_snode->is_place()) {
      emit(R"(device {}* {} = {}.get{}().val;)",
           metal_data_type_name(stmt->output_snode->dt), stmt->raw_name(),
           stmt->input_ptr->raw_name(), stmt->chid);
    } else {
      emit(R"({} {} = {}.get{}();)", stmt->output_snode->node_type_name,
           stmt->raw_name(), stmt->input_ptr->raw_name(), stmt->chid);
    }
  }

  void visit(LinearizeStmt *stmt) override {
    std::string val = "0";
    for (int i = 0; i < (int)stmt->inputs.size(); i++) {
      val = fmt::format("({} * {} + {})", val, stmt->strides[i],
                        stmt->inputs[i]->raw_name());
    }
    emit(R"(auto {} = {};)", stmt->raw_name(), val);
  }

  void visit(OffsetAndExtractBitsStmt *stmt) override {
    emit(R"(auto {} = ((({} + {}) >> {}) & ((1 << {}) - 1));)",
         stmt->raw_name(), stmt->offset, stmt->input->raw_name(),
         stmt->bit_begin, stmt->bit_end - stmt->bit_begin);
  }

  void visit(SNodeLookupStmt *stmt) override {
    std::string parent;
    if (stmt->input_snode) {
      parent = stmt->input_snode->raw_name();
    } else {
      TI_ASSERT(root_stmt_ != nullptr);
      parent = root_stmt_->raw_name();
    }
    const auto *sn = stmt->snode;
    const std::string index_name = stmt->input_index->raw_name();
    emit(R"({}_ch {} = {}.children({});)", sn->node_type_name, stmt->raw_name(),
         parent, index_name);
    if (stmt->activate) {
      TI_ASSERT(sn->type == SNodeType::dense && sn->_bitmasked);
      emit("{{");
      {
        ScopedIndent s(line_appender_);
        line_appender_.append_raw(make_snode_meta_bm(sn, "sn_meta"));
        emit("activate({}.addr(), sn_meta, {});", stmt->raw_name(), index_name);
      }
      emit("}}");
    }
  }

  void visit(SNodeOpStmt *stmt) override {
    if (stmt->op_type == SNodeOpType::is_active) {
      emit("int {};", stmt->raw_name());
    }
    emit("{{");
    {
      ScopedIndent s(line_appender_);
      line_appender_.append_raw(make_snode_meta_bm(stmt->snode, "sn_meta"));
      const std::string ch_id = stmt->val->raw_name();
      const std::string ch_addr =
          fmt::format("{}.children({}).addr()", stmt->ptr->raw_name(), ch_id);
      if (stmt->op_type == SNodeOpType::is_active) {
        // is_active(device byte *addr, SNodeMeta meta, int i);
        emit("{} = is_active({}, sn_meta, {});", stmt->raw_name(), ch_addr,
             ch_id);
      } else if (stmt->op_type == SNodeOpType::deactivate) {
        // deactivate(device byte *addr, SNodeMeta meta, int i);
        emit("deactivate({}, sn_meta, {});", ch_addr, ch_id);
      } else {
        TI_NOT_IMPLEMENTED
      }
    }
    emit("}}");
  }

  void visit(GlobalStoreStmt *stmt) override {
    TI_ASSERT(stmt->width() == 1);
    emit(R"(*{} = {};)", stmt->ptr->raw_name(), stmt->data->raw_name());
  }

  void visit(GlobalLoadStmt *stmt) override {
    TI_ASSERT(stmt->width() == 1);
    emit(R"({} {} = *{};)", metal_data_type_name(stmt->element_type()),
         stmt->raw_name(), stmt->ptr->raw_name());
  }

  void visit(ArgLoadStmt *stmt) override {
    const auto dt = metal_data_type_name(stmt->element_type());
    if (stmt->is_ptr) {
      emit("device {} *{} = {}.arg{}();", dt, stmt->raw_name(),
           kArgsContextName, stmt->arg_id);
    } else {
      emit("const {} {} = *{}.arg{}();", dt, stmt->raw_name(), kArgsContextName,
           stmt->arg_id);
    }
  }

  void visit(ArgStoreStmt *stmt) override {
    const auto dt = metal_data_type_name(stmt->element_type());
    TI_ASSERT(!stmt->is_ptr);
    emit("*{}.arg{}() = {};", kArgsContextName, stmt->arg_id,
         stmt->val->raw_name());
  }

  void visit(ExternalPtrStmt *stmt) override {
    // Used mostly for transferring data between host (e.g. numpy array) and
    // Metal.
    TI_ASSERT(stmt->width() == 1);
    const auto linear_index_name =
        fmt::format("{}_linear_index_", stmt->raw_name());
    emit("int {} = 0;", linear_index_name);
    emit("{{");
    {
      ScopedIndent s(line_appender_);
      const auto *argload = stmt->base_ptrs[0]->as<ArgLoadStmt>();
      const int arg_id = argload->arg_id;
      const int num_indices = stmt->indices.size();
      std::vector<std::string> size_var_names;
      for (int i = 0; i < num_indices; i++) {
        std::string var_name = fmt::format("{}_size{}_", stmt->raw_name(), i);
        emit("const int {} = {}.extra_arg({}, {});", var_name, kArgsContextName,
             arg_id, i);
        size_var_names.push_back(std::move(var_name));
      }
      for (int i = 0; i < num_indices; i++) {
        emit("{} *= {};", linear_index_name, size_var_names[i]);
        emit("{} += {};", linear_index_name, stmt->indices[i]->raw_name());
      }
    }
    emit("}}");

    const auto dt = metal_data_type_name(stmt->element_type());
    emit("device {} *{} = ({} + {});", dt, stmt->raw_name(),
         stmt->base_ptrs[0]->raw_name(), linear_index_name);
  }

  void visit(GlobalTemporaryStmt *stmt) override {
    TI_ASSERT(stmt->width() == 1);
    const auto dt = metal_data_type_name(stmt->element_type());
    emit("device {}* {} = reinterpret_cast<device {}*>({} + {});", dt,
         stmt->raw_name(), dt, kGlobalTmpsBufferName, stmt->offset);
  }

  void visit(LoopIndexStmt *stmt) override {
    using TaskType = OffloadedStmt::TaskType;
    const auto type = current_kernel_attribs_->task_type;
    const auto stmt_name = stmt->raw_name();
    if (type == TaskType::range_for) {
      TI_ASSERT(stmt->index == 0);
      if (current_kernel_attribs_->range_for_attribs.const_begin) {
        emit("const int {} = (static_cast<int>({}) + {});", stmt_name,
             kKernelThreadIdName,
             current_kernel_attribs_->range_for_attribs.begin);
      } else {
        auto begin_stmt = inject_load_global_tmp(
            current_kernel_attribs_->range_for_attribs.begin);
        emit("const int {} = (static_cast<int>({}) + {});", stmt_name,
             kKernelThreadIdName, begin_stmt);
      }
    } else if (type == TaskType::struct_for) {
      emit("const int {} = {}.coords[{}];", stmt_name, kListgenElemVarName,
           stmt->index);
    } else {
      TI_NOT_IMPLEMENTED;
    }
  }

  void visit(UnaryOpStmt *stmt) override {
    if (stmt->op_type != UnaryOpType::cast) {
      emit("const {} {} = {}({});", metal_data_type_name(stmt->element_type()),
           stmt->raw_name(), metal_unary_op_type_symbol(stmt->op_type),
           stmt->operand->raw_name());
    } else {
      // cast
      if (stmt->cast_by_value) {
        emit("const {} {} = static_cast<{}>({});",
             metal_data_type_name(stmt->element_type()), stmt->raw_name(),
             metal_data_type_name(stmt->cast_type), stmt->operand->raw_name());
      } else {
        // reinterpret the bit pattern
        const auto to_type = to_metal_type(stmt->cast_type);
        const auto to_type_name = metal_data_type_name(to_type);
        TI_ASSERT(metal_data_type_bytes(
                      to_metal_type(stmt->operand->element_type())) ==
                  metal_data_type_bytes(to_type));
        emit("const {} {} = union_cast<{}>({});", to_type_name,
             stmt->raw_name(), to_type_name, stmt->operand->raw_name());
      }
    }
  }

  void visit(BinaryOpStmt *bin) override {
    const auto dt_name = metal_data_type_name(bin->element_type());
    const auto lhs_name = bin->lhs->raw_name();
    const auto rhs_name = bin->rhs->raw_name();
    const auto bin_name = bin->raw_name();
    const auto op_type = bin->op_type;
    if (op_type == BinaryOpType::floordiv) {
      if (is_integral(bin->ret_type.data_type)) {
        emit("const {} {} = ifloordiv({}, {});", dt_name, bin_name, lhs_name,
             rhs_name);
      } else {
        emit("const {} {} = floor({} / {});", dt_name, bin_name, lhs_name,
             rhs_name);
      }
      return;
    }
    if (op_type == BinaryOpType::pow && is_integral(bin->ret_type.data_type)) {
      // TODO(k-ye): Make sure the type is not i64?
      emit("const {} {} = pow_i32({}, {});", dt_name, bin_name, lhs_name,
           rhs_name);
      return;
    }
    const auto binop = metal_binary_op_type_symbol(op_type);
    if (is_metal_binary_op_infix(bin->op_type)) {
      emit("const {} {} = ({} {} {});", dt_name, bin_name, lhs_name, binop,
           rhs_name);
    } else {
      // This is a function call
      emit("const {} {} =  {}({}, {});", dt_name, bin_name, binop, lhs_name,
           rhs_name);
    }
  }

  void visit(TernaryOpStmt *tri) override {
    TI_ASSERT(tri->op_type == TernaryOpType::select);
    emit("const {} {} = ({}) ? ({}) : ({});",
         metal_data_type_name(tri->element_type()), tri->raw_name(),
         tri->op1->raw_name(), tri->op2->raw_name(), tri->op3->raw_name());
  }

  void visit(AtomicOpStmt *stmt) override {
    TI_ASSERT(stmt->width() == 1);
    const auto dt = stmt->val->element_type();
    const auto op_type = stmt->op_type;
    std::string op_name;
    bool handle_float = false;
    if (op_type == AtomicOpType::add || op_type == AtomicOpType::min ||
        op_type == AtomicOpType::max) {
      op_name = atomic_op_type_name(op_type);
      handle_float = true;
    } else if (op_type == AtomicOpType::bit_and ||
               op_type == AtomicOpType::bit_or ||
               op_type == AtomicOpType::bit_xor) {
      // Skip "bit_"
      op_name = atomic_op_type_name(op_type).substr(/*pos=*/4);
      handle_float = false;
    } else {
      TI_NOT_IMPLEMENTED;
    }

    if (dt == DataType::i32) {
      emit(
          "const auto {} = atomic_fetch_{}_explicit((device atomic_int*){}, "
          "{}, "
          "metal::memory_order_relaxed);",
          stmt->raw_name(), op_name, stmt->dest->raw_name(),
          stmt->val->raw_name());
    } else if (dt == DataType::u32) {
      emit(
          "const auto {} = atomic_fetch_{}_explicit((device atomic_uint*){}, "
          "{}, "
          "metal::memory_order_relaxed);",
          stmt->raw_name(), op_name, stmt->dest->raw_name(),
          stmt->val->raw_name());
    } else if (dt == DataType::f32) {
      if (handle_float) {
        emit("const float {} = fatomic_fetch_{}({}, {});", stmt->raw_name(),
             op_name, stmt->dest->raw_name(), stmt->val->raw_name());
      } else {
        TI_ERROR("Metal does not support atomic {} for floating points",
                 op_name);
      }
    } else {
      TI_ERROR("Metal only supports 32-bit atomic data types");
    }
  }

  void visit(IfStmt *if_stmt) override {
    emit("if ({}) {{", if_stmt->cond->raw_name());
    if (if_stmt->true_statements) {
      if_stmt->true_statements->accept(this);
    }
    emit("}} else {{");
    if (if_stmt->false_statements) {
      if_stmt->false_statements->accept(this);
    }
    emit("}}");
  }

  void visit(RangeForStmt *for_stmt) override {
    TI_ASSERT(for_stmt->width() == 1);
    auto *loop_var = for_stmt->loop_var;
    if (loop_var->ret_type.data_type == DataType::i32) {
      if (!for_stmt->reversed) {
        emit("for (int {}_ = {}; {}_ < {}; {}_ = {}_ + {}) {{",
             loop_var->raw_name(), for_stmt->begin->raw_name(),
             loop_var->raw_name(), for_stmt->end->raw_name(),
             loop_var->raw_name(), loop_var->raw_name(), 1);
        emit("  int {} = {}_;", loop_var->raw_name(), loop_var->raw_name());
      } else {
        // reversed for loop
        emit("for (int {}_ = {} - 1; {}_ >= {}; {}_ = {}_ - {}) {{",
             loop_var->raw_name(), for_stmt->end->raw_name(),
             loop_var->raw_name(), for_stmt->begin->raw_name(),
             loop_var->raw_name(), loop_var->raw_name(), 1);
        emit("  int {} = {}_;", loop_var->raw_name(), loop_var->raw_name());
      }
    } else {
      TI_ASSERT(!for_stmt->reversed);
      const auto type_name = metal_data_type_name(loop_var->element_type());
      emit("for ({} {} = {}; {} < {}; {} = {} + ({})1) {{", type_name,
           loop_var->raw_name(), for_stmt->begin->raw_name(),
           loop_var->raw_name(), for_stmt->end->raw_name(),
           loop_var->raw_name(), loop_var->raw_name(), type_name);
    }
    for_stmt->body->accept(this);
    emit("}}");
  }

  void visit(StructForStmt *) override {
    TI_ERROR("Struct for cannot be nested.");
  }

  void visit(OffloadedStmt *stmt) override {
    TI_ASSERT(is_top_level_);
    is_top_level_ = false;
    using Type = OffloadedStmt::TaskType;
    if (stmt->task_type == Type::serial) {
      generate_serial_kernel(stmt);
    } else if (stmt->task_type == Type::range_for) {
      generate_range_for_kernel(stmt);
    } else if (stmt->task_type == Type::struct_for) {
      generate_struct_for_kernel(stmt);
    } else if (stmt->task_type == Type::clear_list) {
      add_runtime_list_op_kernel(stmt, "clear_list");
    } else if (stmt->task_type == Type::listgen) {
      add_runtime_list_op_kernel(stmt, "element_listgen");
    } else if (stmt->task_type == Type::gc) {
      // Ignored
    } else {
      // struct_for is automatically lowered to ranged_for for dense snodes
      // (#378). So we only need to support serial and range_for tasks.
      TI_ERROR("Unsupported offload type={} on Metal arch", stmt->task_name());
    }
    is_top_level_ = true;
  }

  void visit(WhileControlStmt *stmt) override {
    emit("if (!{}) break;", stmt->cond->raw_name());
  }

  void visit(WhileStmt *stmt) override {
    emit("while (true) {{");
    stmt->body->accept(this);
    emit("}}");
  }

  void visit(RandStmt *stmt) override {
    TI_ERROR("Metal arch doesn't support ti.random() yet");
  }

  void visit(PrintStmt *stmt) override {
    // TODO: Add a flag to control whether ignoring print() stmt is allowed.
    TI_WARN("Cannot print inside Metal kernel, ignored");
  }

 private:
  void generate_mtl_header() {
    emit("#include <metal_stdlib>");
    emit("using namespace metal;");
    emit("");
    emit("namespace {{");
    emit("");
    emit("using byte = uchar;");
    emit("");
#define TI_INSIDE_METAL_CODEGEN
#include "taichi/platform/metal/shaders/helpers.metal.h"
    line_appender_.append_raw(kMetalHelpersSourceCode);
#undef TI_INSIDE_METAL_CODEGEN
    emit("");
    line_appender_.append_raw(compiled_snodes_->snode_structs_source_code);
    emit("");
    line_appender_.append_raw(compiled_snodes_->runtime_utils_source_code);
    emit("");
    emit("}}  // namespace");
    emit("");
    line_appender_.append_raw(compiled_snodes_->runtime_kernels_source_code);
    emit("");
  }

  void generate_kernel_args_struct() {
    if (args_attribs_.has_args()) {
      const auto class_name = kernel_args_classname();
      emit("namespace {{");
      emit("class {} {{", class_name);
      emit(" public:");
      line_appender_.push_indent();
      emit("explicit {}(device byte* addr) : addr_(addr) {{}}", class_name);
      for (const auto &arg : args_attribs_.args()) {
        const auto dt_name = metal_data_type_name(arg.dt);
        emit("device {}* arg{}() {{", dt_name, arg.index);
        if (arg.is_array) {
          emit("  // array, size={} B", arg.stride);
        } else {
          emit("  // scalar, size={} B", arg.stride);
        }
        emit("  return (device {}*)(addr_ + {});", dt_name, arg.offset_in_mem);
        emit("}}");
      }
      emit("");
      emit("int32_t extra_arg(int i, int j) {{");
      emit("  device int32_t* base = (device int32_t*)(addr_ + {});",
           args_attribs_.args_bytes());
      emit("  return *(base + (i * {}) + j);", taichi_max_num_indices);
      emit("}}");
      line_appender_.pop_indent();
      emit(" private:");
      emit("  device byte* addr_;");
      emit("}};");
      emit("}}  // namespace");
      emit("");
    }
  }

  std::vector<BuffersEnum> get_root_tmps_args_buffers() {
    std::vector<BuffersEnum> result;
    if (needs_root_buffer_) {
      result.push_back(BuffersEnum::Root);
    }
    result.push_back(BuffersEnum::GlobalTmps);
    if (args_attribs_.has_args()) {
      result.push_back(BuffersEnum::Args);
    }
    return result;
  }

  void generate_serial_kernel(OffloadedStmt *stmt) {
    TI_ASSERT(stmt->task_type == OffloadedStmt::TaskType::serial);
    const std::string mtl_kernel_name = make_kernel_name();
    MetalKernelAttributes ka;
    ka.name = mtl_kernel_name;
    ka.task_type = stmt->task_type;
    ka.buffers = get_root_tmps_args_buffers();
    ka.num_threads = 1;

    emit_mtl_kernel_func_sig(mtl_kernel_name, ka.buffers);
    emit("  // serial");
    emit("  if ({} > 0) return;", kKernelThreadIdName);

    current_kernel_attribs_ = &ka;
    stmt->body->accept(this);
    // Close kernel
    emit("}}\n");
    current_kernel_attribs_ = nullptr;

    mtl_kernels_attribs_.push_back(ka);
  }

  void generate_range_for_kernel(OffloadedStmt *stmt) {
    TI_ASSERT(stmt->task_type == OffloadedStmt::TaskType::range_for);
    const std::string mtl_kernel_name = make_kernel_name();
    MetalKernelAttributes ka;
    ka.name = mtl_kernel_name;
    ka.task_type = stmt->task_type;
    ka.buffers = get_root_tmps_args_buffers();

    emit_mtl_kernel_func_sig(mtl_kernel_name, ka.buffers);

    auto &range_for_attribs = ka.range_for_attribs;
    range_for_attribs.const_begin = stmt->const_begin;
    range_for_attribs.const_end = stmt->const_end;
    range_for_attribs.begin =
        (stmt->const_begin ? stmt->begin_value : stmt->begin_offset);
    range_for_attribs.end =
        (stmt->const_end ? stmt->end_value : stmt->end_offset);

    line_appender_.push_indent();
    if (range_for_attribs.const_range()) {
      ka.num_threads = range_for_attribs.end - range_for_attribs.begin;
      emit("// range_for, range known at compile time");
      emit("if ({} >= {}) return;", kKernelThreadIdName, ka.num_threads);
    } else {
      ka.num_threads = -1;
      emit("// range_for, range known at runtime");
      emit("{{");
      {
        ScopedIndent s(line_appender_);
        const auto begin_stmt =
            stmt->const_begin ? std::to_string(stmt->begin_value)
                              : inject_load_global_tmp(stmt->begin_offset);
        const auto end_stmt = stmt->const_end
                                  ? std::to_string(stmt->end_value)
                                  : inject_load_global_tmp(stmt->end_offset);
        emit("if ({} >= ({} - {})) return;", kKernelThreadIdName, end_stmt,
             begin_stmt);
      }
      emit("}}");
    }
    line_appender_.pop_indent();

    current_kernel_attribs_ = &ka;
    stmt->body->accept(this);
    // Close kernel
    emit("}}\n");
    current_kernel_attribs_ = nullptr;

    mtl_kernels_attribs_.push_back(ka);
  }

  void generate_struct_for_kernel(OffloadedStmt *stmt) {
    TI_ASSERT(stmt->task_type == OffloadedStmt::TaskType::struct_for);
    const std::string mtl_kernel_name = make_kernel_name();

    MetalKernelAttributes ka;
    ka.name = mtl_kernel_name;
    ka.task_type = stmt->task_type;
    ka.buffers = get_root_tmps_args_buffers();
    ka.buffers.push_back(BuffersEnum::Runtime);

    emit_mtl_kernel_func_sig(mtl_kernel_name, ka.buffers);

    const int sn_id = stmt->snode->id;
    ka.num_threads = compiled_snodes_->snode_descriptors.find(sn_id)
                         ->second.total_num_elems_from_root;

    line_appender_.push_indent();
    emit("// struct_for");
    emit("device Runtime *{} = reinterpret_cast<device Runtime *>({});",
         kRuntimeVarName, kRuntimeBufferName);
    // Each thread identifies a unique and active ListgenElement. The identified
    // element contains the coords for the loop index at each dimension.
    emit("ListgenElement {};", kListgenElemVarName);
    emit("{{");
    {
      ScopedIndent s(line_appender_);
      emit("device ListManager *sn_list = &({}->snode_lists[{}]);",
           kRuntimeVarName, sn_id);
      emit("if ((int){} >= num_active(sn_list)) return;", kKernelThreadIdName);
      emit(
          "device byte *list_data_addr = reinterpret_cast<device byte *>({} + "
          "1);",
          kRuntimeVarName);
      emit("{} = get<ListgenElement>(sn_list, {}, list_data_addr);",
           kListgenElemVarName, kKernelThreadIdName);
    }
    emit("}}");
    line_appender_.pop_indent();

    current_kernel_attribs_ = &ka;
    stmt->body->accept(this);
    emit("}}\n");
    current_kernel_attribs_ = nullptr;

    mtl_kernels_attribs_.push_back(ka);
  }

  void add_runtime_list_op_kernel(OffloadedStmt *stmt,
                                  const std::string &kernel_name) {
    using Type = OffloadedStmt::TaskType;
    const auto type = stmt->task_type;
    auto *const sn = stmt->snode;
    MetalKernelAttributes ka;
    ka.name = kernel_name;
    ka.task_type = stmt->task_type;
    if (type == Type::clear_list) {
      ka.num_threads = 1;
      ka.buffers = {BuffersEnum::Runtime, BuffersEnum::Args};
    } else if (type == Type::listgen) {
      // This launches |total_num_elems_from_root| number of threads, which
      // could be a huge waste of GPU resources.
      // TODO(k-ye): use grid-stride loop to reduce #threads.
      ka.num_threads = compiled_snodes_->snode_descriptors.find(sn->id)
                           ->second.total_num_elems_from_root;
      ka.buffers = {BuffersEnum::Runtime, BuffersEnum::Root, BuffersEnum::Args};
    } else {
      TI_ERROR("Unsupported offload task type {}", stmt->task_name());
    }
    ka.runtime_list_op_attribs.snode = sn;
    current_kernel_attribs_ = nullptr;

    mtl_kernels_attribs_.push_back(ka);
  }

  std::string inject_load_global_tmp(int offset, DataType dt = DataType::i32) {
    const auto vt = VectorType(/*width=*/1, dt);
    auto gtmp = Stmt::make<GlobalTemporaryStmt>(offset, vt);
    gtmp->accept(this);
    auto gload = Stmt::make<GlobalLoadStmt>(gtmp.get());
    gload->ret_type = vt;
    gload->accept(this);
    return gload->raw_name();
  }

  std::string make_snode_meta_bm(const SNode *sn,
                                 const std::string &var_name) const {
    TI_ASSERT(sn->type == SNodeType::dense && sn->_bitmasked);
    const auto &meta = compiled_snodes_->snode_descriptors.find(sn->id)->second;
    LineAppender la = line_appender_;
    // Keep the indentation settings only
    la.clear_lines();

    la.append("SNodeMeta {};", var_name);
    la.append("{}.element_stride = {};", var_name, meta.element_stride);
    la.append("{}.num_slots = {};", var_name, meta.num_slots);
    la.append("{}.type = {};", var_name, meta.num_slots);
    return la.lines();
  }

  std::string make_kernel_name() {
    return fmt::format("{}_{}", mtl_kernel_prefix_, mtl_kernel_count_++);
  }

  inline std::string kernel_args_classname() const {
    return fmt::format("{}_args", mtl_kernel_prefix_);
  }

  void emit_mtl_kernel_func_sig(
      const std::string &kernel_name,
      const std::vector<MetalKernelAttributes::Buffers> &buffers) {
    auto buffer_to_name = [](BuffersEnum b) -> std::string {
      switch (b) {
        case BuffersEnum::Root:
          return kRootBufferName;
        case BuffersEnum::GlobalTmps:
          return kGlobalTmpsBufferName;
        case BuffersEnum::Args:
          return kArgsBufferName;
        case BuffersEnum::Runtime:
          return kRuntimeBufferName;
        default:
          TI_NOT_IMPLEMENTED;
          break;
      }
      return {};
    };
    emit("kernel void {}(", kernel_name);
    for (int i = 0; i < buffers.size(); ++i) {
      emit("    device byte* {} [[buffer({})]],", buffer_to_name(buffers[i]),
           i);
    }
    emit("    const uint {} [[thread_position_in_grid]]) {{",
         kKernelThreadIdName);
    if (args_attribs_.has_args()) {
      emit("  {} {}({});", kernel_args_classname(), kArgsContextName,
           kArgsBufferName);
    }
  }

  template <typename... Args>
  void emit(std::string f, Args &&... args) {
    line_appender_.append(std::move(f), std::move(args)...);
  }

  const std::string mtl_kernel_prefix_;
  const std::string root_snode_type_name_;
  Kernel *const kernel_;
  const StructCompiledResult *const compiled_snodes_;
  const bool needs_root_buffer_;
  const MetalKernelArgsAttributes args_attribs_;

  bool is_top_level_{true};
  int mtl_kernel_count_{0};
  std::vector<MetalKernelAttributes> mtl_kernels_attribs_;
  GetRootStmt *root_stmt_{nullptr};
  MetalKernelAttributes *current_kernel_attribs_{nullptr};
  LineAppender line_appender_;
};

}  // namespace

MetalCodeGen::MetalCodeGen(const std::string &kernel_name,
                           const StructCompiledResult *struct_compiled)
    : id_(Program::get_kernel_id()),
      taichi_kernel_name_(fmt::format("mtl_k{:04d}_{}", id_, kernel_name)),
      struct_compiled_(struct_compiled) {
}

FunctionType MetalCodeGen::compile(Program &,
                                   Kernel &kernel,
                                   MetalRuntime *runtime) {
  this->prog_ = &kernel.program;
  this->kernel_ = &kernel;
  lower();
  return gen(*prog_->snode_root, runtime);
}

void MetalCodeGen::lower() {
  auto ir = kernel_->ir;
  const bool print_ir = prog_->config.print_ir;
  if (print_ir) {
    TI_TRACE("Initial IR:");
    irpass::print(ir);
  }

  if (kernel_->grad) {
    irpass::reverse_segments(ir);
    irpass::re_id(ir);
    if (print_ir) {
      TI_TRACE("Segment reversed (for autodiff):");
      irpass::print(ir);
    }
  }

  irpass::lower(ir);
  irpass::re_id(ir);
  if (print_ir) {
    TI_TRACE("Lowered:");
    irpass::print(ir);
  }

  irpass::typecheck(ir);
  irpass::re_id(ir);
  if (print_ir) {
    TI_TRACE("Typechecked:");
    irpass::print(ir);
  }

  irpass::demote_dense_struct_fors(ir);
  irpass::typecheck(ir);
  if (print_ir) {
    TI_TRACE("Dense Struct-for demoted:");
    irpass::print(ir);
  }

  irpass::constant_fold(ir);
  if (prog_->config.simplify_before_lower_access) {
    irpass::simplify(ir);
    irpass::re_id(ir);
    if (print_ir) {
      TI_TRACE("Simplified I:");
      irpass::print(ir);
    }
  }

  if (kernel_->grad) {
    irpass::demote_atomics(ir);
    irpass::full_simplify(ir, prog_->config);
    irpass::typecheck(ir);
    if (print_ir) {
      TI_TRACE("Before make_adjoint:");
      irpass::print(ir);
    }
    irpass::make_adjoint(ir);
    if (print_ir) {
      TI_TRACE("After make_adjoint:");
      irpass::print(ir);
    }
    irpass::typecheck(ir);
  }

  irpass::lower_access(ir, prog_->config.use_llvm);
  irpass::re_id(ir);
  if (print_ir) {
    TI_TRACE("Access Lowered:");
    irpass::print(ir);
  }

  irpass::die(ir);
  irpass::re_id(ir);
  if (print_ir) {
    TI_TRACE("DIEd:");
    irpass::print(ir);
  }

  irpass::flag_access(ir);
  irpass::re_id(ir);
  if (print_ir) {
    TI_TRACE("Access Flagged:");
    irpass::print(ir);
  }

  irpass::constant_fold(ir);
  if (print_ir) {
    TI_TRACE("Constant folded:");
    irpass::re_id(ir);
    irpass::print(ir);
  }

  global_tmps_buffer_size_ =
      std::max(irpass::offload(ir).total_size, (size_t)(1));
  if (print_ir) {
    TI_TRACE("Offloaded:");
    irpass::re_id(ir);
    irpass::print(ir);
  }

  irpass::full_simplify(ir, prog_->config);
  if (print_ir) {
    TI_TRACE("Simplified II:");
    irpass::re_id(ir);
    irpass::print(ir);
  }

  irpass::demote_atomics(ir);
  if (print_ir) {
    TI_TRACE("Atomics demoted:");
    irpass::re_id(ir);
    irpass::print(ir);
  }
}

FunctionType MetalCodeGen::gen(const SNode &root_snode, MetalRuntime *runtime) {
  // Make a copy of the name!
  const std::string taichi_kernel_name = taichi_kernel_name_;
  MetalKernelCodegen codegen(taichi_kernel_name, root_snode.node_type_name,
                             kernel_, struct_compiled_);
  codegen.run();
  runtime->register_taichi_kernel(
      taichi_kernel_name, codegen.kernel_source_code(),
      codegen.kernels_attribs(), global_tmps_buffer_size_,
      codegen.kernel_args_attribs());
  return [runtime, taichi_kernel_name](Context &ctx) {
    runtime->launch_taichi_kernel(taichi_kernel_name, &ctx);
  };
}

}  // namespace metal
TLANG_NAMESPACE_END
