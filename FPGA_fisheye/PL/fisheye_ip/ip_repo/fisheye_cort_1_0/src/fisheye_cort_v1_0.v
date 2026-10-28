module fisheye_cort_v1_0 ( // 鱼眼校正 IP 顶层：AXI-Lite 配置，128 位 DDR 读写和 RGBX AXIS 输出。
    input wire aclk,
    input wire aresetn,
    input wire [31:0] dec_dst_base,
    input wire [10:0] dec_rows_committed,
    input wire [7:0] dec_generation,
    input wire dec_active,
    input wire dec_done,
    input wire dec_error,
    input wire [31:0] dec_rear_dst_base,
    input wire [10:0] dec_rear_rows_committed,
    input wire [7:0] dec_rear_generation,
    input wire dec_rear_active,
    input wire dec_rear_done,
    input wire dec_rear_error,
    input wire [6:0] s00_axi_awaddr,
    input wire [2:0] s00_axi_awprot,
    input wire s00_axi_awvalid,
    output wire s00_axi_awready,
    input wire [31:0] s00_axi_wdata,
    input wire [3:0] s00_axi_wstrb,
    input wire s00_axi_wvalid,
    output wire s00_axi_wready,
    output wire [1:0] s00_axi_bresp,
    output wire s00_axi_bvalid,
    input wire s00_axi_bready,
    input wire [6:0] s00_axi_araddr,
    input wire [2:0] s00_axi_arprot,
    input wire s00_axi_arvalid,
    output wire s00_axi_arready,
    output wire [31:0] s00_axi_rdata,
    output wire [1:0] s00_axi_rresp,
    output wire s00_axi_rvalid,
    input wire s00_axi_rready,
    output wire [31:0] m_axis_tdata,
    output wire [3:0] m_axis_tkeep,
    output wire m_axis_tlast,
    output wire m_axis_tvalid,
    input wire m_axis_tready,
    output wire [31:0] m00_axi_awaddr,
    output wire [7:0] m00_axi_awlen,
    output wire [2:0] m00_axi_awsize,
    output wire [1:0] m00_axi_awburst,
    output wire m00_axi_awlock,
    output wire [3:0] m00_axi_awcache,
    output wire [2:0] m00_axi_awprot,
    output wire [3:0] m00_axi_awqos,
    output wire m00_axi_awvalid,
    input wire m00_axi_awready,
    output wire [127:0] m00_axi_wdata,
    output wire [15:0] m00_axi_wstrb,
    output wire m00_axi_wlast,
    output wire m00_axi_wvalid,
    input wire m00_axi_wready,
    input wire [1:0] m00_axi_bresp,
    input wire m00_axi_bvalid,
    output wire m00_axi_bready,
    (* X_INTERFACE_INFO="xilinx.com:interface:aximm:1.0 M00_AXI ARADDR" *)
    (* X_INTERFACE_PARAMETER="XIL_INTERFACENAME M00_AXI, NUM_READ_OUTSTANDING 4, NUM_WRITE_OUTSTANDING 1, MAX_BURST_LENGTH 128" *)
    output wire [31:0] m00_axi_araddr,
    output wire [7:0] m00_axi_arlen,
    output wire [2:0] m00_axi_arsize,
    output wire [1:0] m00_axi_arburst,
    output wire m00_axi_arlock,
    output wire [3:0] m00_axi_arcache,
    output wire [2:0] m00_axi_arprot,
    output wire [3:0] m00_axi_arqos,
    output wire m00_axi_arvalid,
    input wire m00_axi_arready,
    input wire [127:0] m00_axi_rdata,
    input wire [1:0] m00_axi_rresp,
    input wire m00_axi_rlast,
    input wire m00_axi_rvalid,
    output wire m00_axi_rready
);
    reg aw_pending, w_pending, b_valid, r_valid; // AXI-Lite 的 AW/W 独立捕获，二者均到达后执行一次寄存器写入。
    reg [6:0] aw_hold;
    reg [31:0] w_hold, r_hold;
    reg [3:0] strb_hold;
    reg [31:0] cfg_src, cfg_x, cfg_y, cfg_dst; // 字节偏移：0x00 启动，0x04 状态，0x08/0x0C/0x10/0x14 前向源/X/Y/目标基址。
    reg [31:0] cfg_rear_src, cfg_rear_x, cfg_rear_y, cfg_weight; // 0x40/0x44/0x48/0x4C 为后向源/X/Y/融合权重基址。
    reg [31:0] frame_rear_src, frame_rear_x, frame_rear_y, frame_weight;
    reg [31:0] cfg_center_x, cfg_center_y, cfg_pipe; // 0x24 控制：bit0 等待解码水位，bit1 启用 AXIS；中心参数保留旧接口兼容。
    reg [31:0] frame_src, frame_x, frame_y, frame_dst;
    reg [10:0] frame_crop_x, frame_crop_y;
    reg frame_wait_source, frame_stream_enable, crop_clamped;
    reg start_pulse, core_start, busy, done_sticky, fault;
    reg producer_fault;
    reg [31:0] cycles;
    wire read_error, write_error, read_idle, write_idle, write_done;
    wire contract_warning, contract_error;
    wire stream_done, producer_wait, source_all_ready;
    wire run_enable;
    wire fatal_fault;
    assign fatal_fault = fault || producer_fault || contract_error; // 故障停止核心运行，读写引擎继续排空已提交的 AXI 事务。
    assign run_enable = busy && !fatal_fault;
    wire rd_cmd_valid, rd_cmd_ready, rd_valid, rd_ready, rd_done;
    wire [31:0] rd_addr;
    wire [19:0] rd_beats;
    wire [127:0] rd_data, packed_data, fifo_data;
    wire fifo_push, fifo_pop, fifo_valid;
    wire [10:0] fifo_level;
    wire [31:0] stream_data;
    wire [3:0] stream_keep;
    wire stream_last, stream_valid;
    assign m_axis_tdata = stream_data;
    assign m_axis_tkeep = stream_keep;
    assign m_axis_tlast = stream_last;
    assign m_axis_tvalid = stream_valid;
    wire [31:0] byte_mask;
    assign byte_mask = {{8{strb_hold[3]}},{8{strb_hold[2]}},{8{strb_hold[1]}},{8{strb_hold[0]}}}; // 按 WSTRB 更新配置寄存器，支持独立字节写入。
    assign s00_axi_awready = !aw_pending && !b_valid;
    assign s00_axi_wready = !w_pending && !b_valid;
    assign s00_axi_bvalid = b_valid;
    assign s00_axi_bresp = 2'b00;
    assign s00_axi_arready = !r_valid;
    assign s00_axi_rvalid = r_valid;
    assign s00_axi_rdata = r_hold;
    assign s00_axi_rresp = 2'b00;
    assign m00_axi_awsize = 3'd4;
    assign m00_axi_arsize = 3'd4;
    assign m00_axi_awburst = 2'b01;
    assign m00_axi_arburst = 2'b01;
    assign m00_axi_awlock = 1'b0;
    assign m00_axi_arlock = 1'b0;
    assign m00_axi_awcache = 4'b0010;
    assign m00_axi_arcache = 4'b0010;
    assign m00_axi_awprot = 3'b000;
    assign m00_axi_arprot = 3'b000;
    assign m00_axi_awqos = 4'd0;
    assign m00_axi_arqos = 4'd0;
    assign m00_axi_wstrb = 16'hffff;
    always @(posedge aclk) begin
        if (!aresetn) begin
            aw_pending <= 0; w_pending <= 0; b_valid <= 0; r_valid <= 0; aw_hold <= 0; w_hold <= 0; strb_hold <= 0; r_hold <= 0;
            cfg_src <= 0; cfg_x <= 0; cfg_y <= 0; cfg_dst <= 0; cfg_center_x <= 32'd960; cfg_center_y <= 32'd540; cfg_pipe <= 0; cfg_rear_src <= 0; cfg_rear_x <= 0; cfg_rear_y <= 0; cfg_weight <= 0; start_pulse <= 0;
        end else begin
            start_pulse <= 0;
            if (s00_axi_awvalid && s00_axi_awready) begin aw_pending <= 1; aw_hold <= s00_axi_awaddr; end
            if (s00_axi_wvalid && s00_axi_wready) begin w_pending <= 1; w_hold <= s00_axi_wdata; strb_hold <= s00_axi_wstrb; end
            if (b_valid && s00_axi_bready) b_valid <= 0;
            if (aw_pending && w_pending && !b_valid) begin
                aw_pending <= 0; w_pending <= 0; b_valid <= 1;
                if (aw_hold[6:2] == 5'd0 && strb_hold[0] && w_hold[0] && !busy && !start_pulse) start_pulse <= 1;
                case (aw_hold[6:2])
                    5'd2: cfg_src <= (cfg_src & ~byte_mask) | (w_hold & byte_mask);
                    5'd3: cfg_x <= (cfg_x & ~byte_mask) | (w_hold & byte_mask);
                    5'd4: cfg_y <= (cfg_y & ~byte_mask) | (w_hold & byte_mask);
                    5'd5: cfg_dst <= (cfg_dst & ~byte_mask) | (w_hold & byte_mask);
                    5'd7: cfg_center_x <= (cfg_center_x & ~byte_mask) | (w_hold & byte_mask);
                    5'd8: cfg_center_y <= (cfg_center_y & ~byte_mask) | (w_hold & byte_mask);
                    5'd9: cfg_pipe <= (cfg_pipe & ~byte_mask) | (w_hold & byte_mask);
                    5'd16: cfg_rear_src <= (cfg_rear_src & ~byte_mask) | (w_hold & byte_mask);
                    5'd17: cfg_rear_x <= (cfg_rear_x & ~byte_mask) | (w_hold & byte_mask);
                    5'd18: cfg_rear_y <= (cfg_rear_y & ~byte_mask) | (w_hold & byte_mask);
                    5'd19: cfg_weight <= (cfg_weight & ~byte_mask) | (w_hold & byte_mask);
                    default: begin end
                endcase
            end
            if (r_valid && s00_axi_rready) r_valid <= 0;
            if (s00_axi_arvalid && s00_axi_arready) begin
                r_valid <= 1;
                case (s00_axi_araddr[6:2])
                    5'd0: r_hold <= 0;
                    5'd1: r_hold <= {23'd0,contract_error,producer_fault,crop_clamped,producer_wait,stream_done,contract_warning,fault,done_sticky,busy}; // 状态低位：busy、done、fault、warning、stream_done、producer_wait、crop_clamped、producer_fault、contract_error。
                    5'd2: r_hold <= cfg_src;
                    5'd3: r_hold <= cfg_x;
                    5'd4: r_hold <= cfg_y;
                    5'd5: r_hold <= cfg_dst;
                    5'd6: r_hold <= cycles;
                    5'd7: r_hold <= cfg_center_x;
                    5'd8: r_hold <= cfg_center_y;
                    5'd9: r_hold <= cfg_pipe;
                    5'd10: r_hold <= {5'd0,frame_crop_y,5'd0,frame_crop_x};
                    5'd11: r_hold <= {dec_generation,5'd0,(dec_dst_base == frame_src),dec_done,dec_active,5'd0,dec_rows_committed};
                    5'd12: r_hold <= {16'd1328,16'd1328};
                    5'd13: r_hold <= {16'd1328,16'd2656};
                    5'd14: r_hold <= 32'd384;
                    5'd15: r_hold <= 32'd4;
                    5'd16: r_hold <= cfg_rear_src;
                    5'd17: r_hold <= cfg_rear_x;
                    5'd18: r_hold <= cfg_rear_y;
                    5'd19: r_hold <= cfg_weight;
                    5'd20: r_hold <= {dec_rear_generation,5'd0,(dec_rear_dst_base == frame_rear_src),dec_rear_done,dec_rear_active,5'd0,dec_rear_rows_committed};
                    5'd21: r_hold <= 32'd2;
                    5'd22: r_hold <= 32'd1;
                    5'd23: r_hold <= 32'd1;
                    default: r_hold <= 0;
                endcase
            end
        end
    end
    always @(posedge aclk) begin
        if (!aresetn) begin core_start <= 0; busy <= 0; done_sticky <= 0; fault <= 0; producer_fault <= 0; cycles <= 0; frame_src <= 0; frame_x <= 0; frame_y <= 0; frame_dst <= 0; frame_rear_src <= 0; frame_rear_x <= 0; frame_rear_y <= 0; frame_weight <= 0; frame_crop_x <= 11'd0; frame_crop_y <= 11'd0; frame_wait_source <= 0; frame_stream_enable <= 0; crop_clamped <= 0; end
        else if (start_pulse) begin // 启动时锁存本帧参数，运行中的 PS 配置写入不影响当前帧。
            core_start <= 1; busy <= 1; done_sticky <= 0; fault <= 0; producer_fault <= 0; cycles <= 0; frame_src <= cfg_src; frame_x <= cfg_x; frame_y <= cfg_y; frame_dst <= cfg_dst; frame_rear_src <= cfg_rear_src; frame_rear_x <= cfg_rear_x; frame_rear_y <= cfg_rear_y; frame_weight <= cfg_weight; frame_crop_x <= 11'd0; frame_crop_y <= 11'd0; frame_wait_source <= cfg_pipe[0]; frame_stream_enable <= cfg_pipe[1]; crop_clamped <= 1'b0;
        end else begin
            core_start <= 0;
            if (busy) cycles <= cycles+1'b1;
            if (read_error || write_error) fault <= 1;
            if (busy && frame_wait_source && ((dec_error && dec_dst_base == frame_src) || (dec_rear_error && dec_rear_dst_base == frame_rear_src))) producer_fault <= 1;
            if (write_done && !fatal_fault && !read_error) begin busy <= 0; done_sticky <= 1; end // 完成状态以 DDR 写回结束为准；AXIS 流完成单独报告。
            if (fatal_fault && read_idle && write_idle) busy <= 0; // 故障时等待读写总线全部空闲，再释放 busy。
        end
    end
    fc_core u_core (
        .clk(aclk),
        .rst_n(aresetn),
        .start(core_start),
        .enable(run_enable),
        .src_base(frame_src),
        .x_base(frame_x),
        .y_base(frame_y),
        .rear_src_base(frame_rear_src), .rear_x_base(frame_rear_x), .rear_y_base(frame_rear_y), .weight_base(frame_weight),
        .rear_producer_dst_base(dec_rear_dst_base), .rear_producer_rows(dec_rear_rows_committed), .rear_producer_generation(dec_rear_generation),
        .rear_producer_active(dec_rear_active), .rear_producer_done(dec_rear_done),
        .crop_x(frame_crop_x),
        .crop_y(frame_crop_y),
        .wait_source(frame_wait_source),
        .producer_dst_base(dec_dst_base),
        .producer_rows(dec_rows_committed),
        .producer_generation(dec_generation),
        .producer_active(dec_active),
        .producer_done(dec_done),
        .cmd_valid(rd_cmd_valid),
        .cmd_ready(rd_cmd_ready),
        .cmd_addr(rd_addr),
        .cmd_beats(rd_beats),
        .data_valid(rd_valid),
        .data_ready(rd_ready),
        .data(rd_data),
        .fifo_level(fifo_level),
        .fifo_push(fifo_push),
        .fifo_data(packed_data),
        .stream_enable(frame_stream_enable),
        .stream_data(stream_data),
        .stream_keep(stream_keep),
        .stream_last(stream_last),
        .stream_valid(stream_valid),
        .stream_ready(m_axis_tready),
        .stream_done(stream_done),
        .producer_wait(producer_wait),
        .source_all_ready(source_all_ready),
        .contract_warning(contract_warning),
        .contract_error(contract_error)
    );
    fc_reader u_reader (
        .clk(aclk),
        .rst_n(aresetn),
        .abort(fatal_fault),
        .cmd_valid(rd_cmd_valid),
        .cmd_ready(rd_cmd_ready),
        .cmd_addr(rd_addr),
        .cmd_beats(rd_beats),
        .data_valid(rd_valid),
        .data_ready(rd_ready),
        .data(rd_data),
        .done(rd_done),
        .error(read_error),
        .idle(read_idle),
        .araddr(m00_axi_araddr),
        .arlen(m00_axi_arlen),
        .arvalid(m00_axi_arvalid),
        .arready(m00_axi_arready),
        .rdata(m00_axi_rdata),
        .rresp(m00_axi_rresp),
        .rlast(m00_axi_rlast),
        .rvalid(m00_axi_rvalid),
        .rready(m00_axi_rready)
    );
    fc_fifo u_fifo ( // 计算结果经 FIFO 解耦后写回 DDR；AXIS 输出同时受公共背压控制。
        .clk(aclk),
        .rst_n(aresetn),
        .clear(core_start),
        .push(fifo_push),
        .din(packed_data),
        .pop(fifo_pop),
        .valid(fifo_valid),
        .dout(fifo_data),
        .level(fifo_level)
    );
    fc_writer u_writer (
        .clk(aclk),
        .rst_n(aresetn),
        .start(core_start),
        .abort(fatal_fault),
        .base(frame_dst),
        .total_beats(20'd881792), // 2656*1328*4/16 = 881792 个 128 位 RGBX 数据拍。
        .level(fifo_level),
        .valid(fifo_valid),
        .data(fifo_data),
        .pop(fifo_pop),
        .done(write_done),
        .error(write_error),
        .idle(write_idle),
        .awaddr(m00_axi_awaddr),
        .awlen(m00_axi_awlen),
        .awvalid(m00_axi_awvalid),
        .awready(m00_axi_awready),
        .wdata(m00_axi_wdata),
        .wvalid(m00_axi_wvalid),
        .wready(m00_axi_wready),
        .wlast(m00_axi_wlast),
        .bresp(m00_axi_bresp),
        .bvalid(m00_axi_bvalid),
        .bready(m00_axi_bready)
    );
endmodule
