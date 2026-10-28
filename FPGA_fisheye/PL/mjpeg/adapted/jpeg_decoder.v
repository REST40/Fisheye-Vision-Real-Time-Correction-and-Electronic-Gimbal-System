//-----------------------------------------------------------------
//                       AXI-4 JPEG Decoder
//                             V0.2
//                       Ultra-Embedded.com
//                        Copyright 2020
//
//                   admin@ultra-embedded.com
//-----------------------------------------------------------------
//                      License: Apache 2.0
// This IP can be freely used in commercial projects, however you may
// want access to unreleased materials such as verification environments,
// or test vectors, as well as changes to the IP for integration purposes.
// If this is the case, contact the above address.
// I am interested to hear how and where this IP is used, so please get
// in touch!
//-----------------------------------------------------------------
// Copyright 2020 Ultra-Embedded.com
// 
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// 
//     http://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//-----------------------------------------------------------------

// 寄存器常量在本文件展开，避免 BD 接口分析依赖全局宏。

//-----------------------------------------------------------------
// Module:  JPEG Decoder Peripheral
//-----------------------------------------------------------------
module jpeg_decoder
//-----------------------------------------------------------------
// Params
//-----------------------------------------------------------------
#(
     parameter AXI_ID           = 0
)
//-----------------------------------------------------------------
// Ports
//-----------------------------------------------------------------
(
    // 输入接口
     input          clk_i
    ,input          rst_i
    ,input          cfg_awvalid_i
    ,input  [31:0]  cfg_awaddr_i
    ,input          cfg_wvalid_i
    ,input  [31:0]  cfg_wdata_i
    ,input  [3:0]   cfg_wstrb_i
    ,input          cfg_bready_i
    ,input          cfg_arvalid_i
    ,input  [31:0]  cfg_araddr_i
    ,input          cfg_rready_i
    ,input          outport_awready_i
    ,input          outport_wready_i
    ,input          outport_bvalid_i
    ,input  [1:0]   outport_bresp_i
    ,input  [3:0]   outport_bid_i
    ,input          outport_arready_i
    ,input          outport_rvalid_i
    ,input  [31:0]  outport_rdata_i
    ,input  [1:0]   outport_rresp_i
    ,input  [3:0]   outport_rid_i
    ,input          outport_rlast_i

    // 输出接口
    ,output         cfg_awready_o
    ,output         cfg_wready_o
    ,output         cfg_bvalid_o
    ,output [1:0]   cfg_bresp_o
    ,output         cfg_arready_o
    ,output         cfg_rvalid_o
    ,output [31:0]  cfg_rdata_o
    ,output [1:0]   cfg_rresp_o
    ,output         outport_awvalid_o
    ,output [31:0]  outport_awaddr_o
    ,output [3:0]   outport_awid_o
    ,output [7:0]   outport_awlen_o
    ,output [1:0]   outport_awburst_o
    ,output         outport_wvalid_o
    ,output [31:0]  outport_wdata_o
    ,output [3:0]   outport_wstrb_o
    ,output         outport_wlast_o
    ,output         outport_bready_o
    ,output         outport_arvalid_o
    ,output [31:0]  outport_araddr_o
    ,output [3:0]   outport_arid_o
    ,output [7:0]   outport_arlen_o
    ,output [1:0]   outport_arburst_o
    ,output         outport_rready_o
    ,output [31:0]  progress_dst_base_o
    ,output [10:0]  progress_rows_committed_o
    ,output [7:0]   progress_generation_o
    ,output         progress_active_o
    ,output         progress_done_o
    ,output         progress_error_o
);

// 寄存器与状态常量集中声明，兼容仿真和综合工具。
reg awvalid_q;
reg wvalid_q;
reg [7:0] wr_addr_q;
reg [31:0] wr_data_q;
reg jpeg_ctrl_wr_q;
reg        jpeg_ctrl_start_q;
reg        jpeg_ctrl_abort_q;
reg [23:0]  jpeg_ctrl_length_q;
reg jpeg_status_wr_q;
reg jpeg_src_wr_q;
reg [31:0]  jpeg_src_addr_q;
reg jpeg_dst_wr_q;
reg [31:0]  jpeg_dst_addr_q;
reg [31:0] data_r;
reg rvalid_q;
reg [31:0] rd_data_q;
reg bvalid_q;
localparam BUFFER_DEPTH   = 1024;
localparam [15:0] MAX_INPUT_WIDTH = 16'd1920; // 保持现有解码核心的宽度范围，1328 像素输入在此范围内。
localparam [15:0] MAX_INPUT_HEIGHT = 16'd1328; // 扩展高度范围，允许完整的 1328×1328 单镜头图像。
wire [15:0] writes_pending; // 写队列返回真实尚未成功/失败返回的B数量。
reg bus_error;
wire [15:0] reads_pending; // 输入预取返回真实尚未收到RLAST的事务数量。
reg [31:0] decoded_pixels;
reg pixel_error;
reg         core_busy_q;
localparam STATE_W          = 2;
localparam STATE_IDLE       = 2'd0;
localparam STATE_FILL       = 2'd1;
localparam STATE_ACTIVE     = 2'd2;
localparam STATE_DRAIN      = 2'd3;
reg [STATE_W-1:0] state_q;
reg [STATE_W-1:0] next_state_r;
reg  core_active_q;
wire [3:0] burst_pending_w; wire [1:0] prefetch_pending_w; // 保持旧状态内部计数含义
wire prefetch_idle_w,prefetch_error_w,burst_error_w,frame_fault_w,read_stop_w,bad_bid_w; // 错误停止新增事务，已接受事务仍排空
wire [31:0] remaining_q; reg [31:0] frame_dst_addr_q; reg frame_abort_q; // 帧启动锁存目标地址和粘滞取消状态
wire [32:0] destination_end_w; wire destination_valid_w; // 防止合法尺寸图像写地址跨32位地址空间
assign writes_pending={12'd0,burst_pending_w}; assign reads_pending={14'd0,prefetch_pending_w}; // 不再重复维护另一套事务计数
assign bad_bid_w=outport_bvalid_i && outport_bready_o && outport_bid_i!=AXI_ID; // 同一主口的响应ID必须一致
assign frame_fault_w=state_q!=STATE_IDLE && (jpeg_ctrl_abort_out_w || frame_abort_q || bus_error || pixel_error || prefetch_error_w || burst_error_w || bad_bid_w); // IDLE旧错误不阻止下一合法START
assign read_stop_w=frame_fault_w || state_q==STATE_DRAIN || (state_q==STATE_ACTIVE && core_busy_q && core_idle_w); // EOI时停止未提交输入读取，排空迟到R
assign destination_end_w={1'b0,frame_dst_addr_q}+(({17'd0,pixel_w_w}*{17'd0,pixel_h_w})<<2); // 一帧RGBX写入范围先保留进位
assign destination_valid_w=frame_dst_addr_q[4:0]==5'd0 && !destination_end_w[32]; // 八像素地址须32字节对齐
reg [1:0] pixel_pipe_valid;
wire burst_ready;
wire burst_idle;
wire pixel_step; // 可停顿的原两拍像素流水
assign pixel_step = burst_ready || !pixel_pipe_valid[1]; // 第一级空闲或后端可接受时前进
reg [31:0] pixel_data_stage;
reg [15:0] pixel_x_stage;
reg [31:0] pixel_offset_q;
reg [31:0] pixel_addr_q;
reg [31:0] pixel_data_q;

wire wr_cmd_accepted_w;
wire wr_data_accepted_w;
wire [7:0] wr_addr_w;
wire read_en_w;
wire write_en_w;
wire        jpeg_ctrl_start_out_w;
wire        jpeg_ctrl_abort_out_w;
wire [23:0]  jpeg_ctrl_length_out_w;
wire [31:0]  jpeg_src_addr_out_w;
wire [31:0]  jpeg_dst_addr_out_w;
wire        jpeg_status_busy_in_w;
wire        jpeg_valid_w;
wire [31:0] jpeg_data_w;
wire        jpeg_accept_w;
wire [10:0] fifo_in_level_w;
wire        core_idle_w;
wire fifo_jpeg_valid_w;
wire [15:0] pixel_x_w;
wire [15:0] pixel_y_w;
wire [15:0] pixel_w_w;
wire [15:0] pixel_h_w;
wire        pixel_valid_w;
wire        pixel_geometry_valid_w; // 尺寸合法才允许像素进入 DDR 写通路。
wire        pixel_in_frame_w; // 完整源图像素，不含 MCU 底部填充。
wire        frame_pixels_complete_w; // 最终水位发布前核对实际接收的整帧像素数。
wire [7:0]  pixel_r_w;
wire [7:0]  pixel_g_w;
wire [7:0]  pixel_b_w;
wire        fifo_accept_in_w;
wire        fence_trigger_w;
wire        core_accept_w;
reg [31:0]  progress_dst_base_q;
reg [10:0]  progress_rows_committed_q;
reg [10:0]  progress_next_commit_q;
reg [10:0]  progress_height_q;
reg [31:0]  progress_pixels_expected_q; // 核心在排空前会复位，提前保存 JPEG 头给出的图像面积。
reg [7:0]   progress_generation_q;
reg         progress_fence_q;
reg         progress_done_q;

assign progress_dst_base_o = progress_dst_base_q;
assign progress_rows_committed_o = progress_rows_committed_q;
assign progress_generation_o = progress_generation_q;
assign progress_active_o = state_q != STATE_IDLE;
assign progress_done_o = progress_done_q;
assign progress_error_o = bus_error || pixel_error || prefetch_error_w || burst_error_w; // 包含本周期已检测的通道错误
assign pixel_geometry_valid_w = pixel_w_w != 16'd0 && pixel_w_w <= MAX_INPUT_WIDTH && pixel_w_w[2:0] == 3'd0 && // 八像素突发要求行宽为八的倍数。
                                pixel_h_w != 16'd0 && pixel_h_w <= MAX_INPUT_HEIGHT && pixel_h_w[2:0] == 3'd0; // 十一位行水位可完整表示 1328 行。
assign pixel_in_frame_w = pixel_geometry_valid_w && destination_valid_w && pixel_x_w < pixel_w_w && pixel_y_w < pixel_h_w; // 非法尺寸和填充像素不能写入源图缓冲。
assign frame_pixels_complete_w = progress_height_q != 11'd0 && decoded_pixels == progress_pixels_expected_q; // 空帧或缺像素帧不能发布完整行水位。

//-----------------------------------------------------------------
// 写地址与写数据独立握手
//-----------------------------------------------------------------
// 已接收地址，等待写数据

// 已接收数据，等待写地址

assign wr_cmd_accepted_w = (cfg_awvalid_i && cfg_awready_o) || awvalid_q;
assign wr_data_accepted_w = (cfg_wvalid_i  && cfg_wready_o)  || wvalid_q;

always @ (posedge clk_i or posedge rst_i)
if (rst_i)
    awvalid_q <= 1'b0;
else if (cfg_awvalid_i && cfg_awready_o && !wr_data_accepted_w)
    awvalid_q <= 1'b1;
else if (wr_data_accepted_w)
    awvalid_q <= 1'b0;

always @ (posedge clk_i or posedge rst_i)
if (rst_i)
    wvalid_q <= 1'b0;
else if (cfg_wvalid_i && cfg_wready_o && !wr_cmd_accepted_w)
    wvalid_q <= 1'b1;
else if (wr_cmd_accepted_w)
    wvalid_q <= 1'b0;

//-----------------------------------------------------------------
// 缓存提前到达的写地址
//-----------------------------------------------------------------

always @ (posedge clk_i or posedge rst_i)
if (rst_i)
    wr_addr_q <= 8'b0;
else if (cfg_awvalid_i && cfg_awready_o)
    wr_addr_q <= cfg_awaddr_i[7:0];

assign wr_addr_w = awvalid_q ? wr_addr_q : cfg_awaddr_i[7:0];

//-----------------------------------------------------------------
// 缓存提前到达的写数据
//-----------------------------------------------------------------

always @ (posedge clk_i or posedge rst_i)
if (rst_i)
    wr_data_q <= 32'b0;
else if (cfg_wvalid_i && cfg_wready_o)
    wr_data_q <= cfg_wdata_i;

//-----------------------------------------------------------------
// Request Logic
//-----------------------------------------------------------------
assign read_en_w = cfg_arvalid_i & cfg_arready_o;
assign write_en_w = wr_cmd_accepted_w && wr_data_accepted_w;

//-----------------------------------------------------------------
// Accept Logic
//-----------------------------------------------------------------
assign cfg_arready_o = ~cfg_rvalid_o;
assign cfg_awready_o = ~cfg_bvalid_o && ~cfg_arvalid_i && ~awvalid_q;
assign cfg_wready_o  = ~cfg_bvalid_o && ~cfg_arvalid_i && ~wvalid_q;


//-----------------------------------------------------------------
// Register jpeg_ctrl
//-----------------------------------------------------------------

always @ (posedge clk_i or posedge rst_i)
if (rst_i)
    jpeg_ctrl_wr_q <= 1'b0;
else if (write_en_w && (wr_addr_w[7:0] == 8'h0))
    jpeg_ctrl_wr_q <= 1'b1;
else
    jpeg_ctrl_wr_q <= 1'b0;

// jpeg_ctrl_start [auto_clr]

always @ (posedge clk_i or posedge rst_i)
if (rst_i)
    jpeg_ctrl_start_q <= 1'd0;
else if (write_en_w && (wr_addr_w[7:0] == 8'h0))
    jpeg_ctrl_start_q <= cfg_wdata_i[31:31];
else
    jpeg_ctrl_start_q <= 1'd0;

assign jpeg_ctrl_start_out_w = jpeg_ctrl_start_q && !jpeg_ctrl_abort_q && state_q==STATE_IDLE && burst_idle && prefetch_idle_w && pixel_pipe_valid==2'b00; // 旧帧完全排空后才接受下一代次


// jpeg_ctrl_abort [auto_clr]

always @ (posedge clk_i or posedge rst_i)
if (rst_i)
    jpeg_ctrl_abort_q <= 1'd0;
else if (write_en_w && (wr_addr_w[7:0] == 8'h0))
    jpeg_ctrl_abort_q <= cfg_wdata_i[30:30];
else
    jpeg_ctrl_abort_q <= 1'd0;

assign jpeg_ctrl_abort_out_w = jpeg_ctrl_abort_q;


// jpeg_ctrl_length [internal]

always @ (posedge clk_i or posedge rst_i)
if (rst_i)
    jpeg_ctrl_length_q <= 24'd0;
else if (write_en_w && (wr_addr_w[7:0] == 8'h0))
    jpeg_ctrl_length_q <= cfg_wdata_i[23:0];

assign jpeg_ctrl_length_out_w = jpeg_ctrl_length_q;


//-----------------------------------------------------------------
// Register jpeg_status
//-----------------------------------------------------------------

always @ (posedge clk_i or posedge rst_i)
if (rst_i)
    jpeg_status_wr_q <= 1'b0;
else if (write_en_w && (wr_addr_w[7:0] == 8'h4))
    jpeg_status_wr_q <= 1'b1;
else
    jpeg_status_wr_q <= 1'b0;


//-----------------------------------------------------------------
// Register jpeg_src
//-----------------------------------------------------------------

always @ (posedge clk_i or posedge rst_i)
if (rst_i)
    jpeg_src_wr_q <= 1'b0;
else if (write_en_w && (wr_addr_w[7:0] == 8'h8))
    jpeg_src_wr_q <= 1'b1;
else
    jpeg_src_wr_q <= 1'b0;

// jpeg_src_addr [internal]

always @ (posedge clk_i or posedge rst_i)
if (rst_i)
    jpeg_src_addr_q <= 32'd0;
else if (write_en_w && (wr_addr_w[7:0] == 8'h8))
    jpeg_src_addr_q <= cfg_wdata_i[31:0];

assign jpeg_src_addr_out_w = jpeg_src_addr_q;


//-----------------------------------------------------------------
// Register jpeg_dst
//-----------------------------------------------------------------

always @ (posedge clk_i or posedge rst_i)
if (rst_i)
    jpeg_dst_wr_q <= 1'b0;
else if (write_en_w && (wr_addr_w[7:0] == 8'hc))
    jpeg_dst_wr_q <= 1'b1;
else
    jpeg_dst_wr_q <= 1'b0;

// jpeg_dst_addr [internal]

always @ (posedge clk_i or posedge rst_i)
if (rst_i)
    jpeg_dst_addr_q <= 32'd0;
else if (write_en_w && (wr_addr_w[7:0] == 8'hc))
    jpeg_dst_addr_q <= cfg_wdata_i[31:0];

assign jpeg_dst_addr_out_w = jpeg_dst_addr_q;




//-----------------------------------------------------------------
// 读取寄存器选择
//-----------------------------------------------------------------

always @ *
begin
    data_r = 32'b0;

    case (cfg_araddr_i[7:0])

    8'h0:
    begin
        data_r[23:0] = jpeg_ctrl_length_q;
    end
    8'h4:
    begin
        data_r[0:0] = jpeg_status_busy_in_w;
        data_r[1] = bus_error;
        data_r[2] = pixel_error;
    end
    8'h8:
    begin
        data_r[31:0] = jpeg_src_addr_q;
    end
    8'hc:
    begin
        data_r[31:0] = jpeg_dst_addr_q;
    end
    8'h10: data_r = decoded_pixels;
    8'h14: data_r = {21'd0,progress_rows_committed_q};
    8'h18: data_r = {21'd0,progress_error_o,progress_done_q,progress_generation_q};
    default :
        data_r = 32'b0;
    endcase
end

//-----------------------------------------------------------------
// RVALID
//-----------------------------------------------------------------

always @ (posedge clk_i or posedge rst_i)
if (rst_i)
    rvalid_q <= 1'b0;
else if (read_en_w)
    rvalid_q <= 1'b1;
else if (cfg_rready_i)
    rvalid_q <= 1'b0;

assign cfg_rvalid_o = rvalid_q;

//-----------------------------------------------------------------
// Retime read response
//-----------------------------------------------------------------

always @ (posedge clk_i or posedge rst_i)
if (rst_i)
    rd_data_q <= 32'b0;
else if (!cfg_rvalid_o || cfg_rready_i)
    rd_data_q <= data_r;

assign cfg_rdata_o = rd_data_q;
assign cfg_rresp_o = 2'b0;

//-----------------------------------------------------------------
// BVALID
//-----------------------------------------------------------------

always @ (posedge clk_i or posedge rst_i)
if (rst_i)
    bvalid_q <= 1'b0;
else if (write_en_w)
    bvalid_q <= 1'b1;
else if (cfg_bready_i)
    bvalid_q <= 1'b0;

assign cfg_bvalid_o = bvalid_q;
assign cfg_bresp_o  = 2'b0;






// 必须等待全部 AXI 写响应，才能向软件报告完成。

// 事务计数由两个专用队列维护，避免无握手的VALID误计数。
always @(posedge clk_i) begin
    if (rst_i || jpeg_ctrl_start_out_w) begin
        decoded_pixels <= 0;
        pixel_error <= 0;
    end else begin // 分别检查实际接收的像素和帧排空时的完整性。
        if (pixel_valid_w && fifo_accept_in_w) begin // 只统计完成握手的输出，背压期间不重复计数。
            if (pixel_in_frame_w) decoded_pixels <= decoded_pixels + 1'b1; // 1328×1328 应接收 1763584 个源图像素。
            // 1328 为十六的倍数，4:2:0 对应 83×83 个 MCU；其余合法高度仍只丢弃底部 MCU 填充。
            if (!pixel_geometry_valid_w || !destination_valid_w || pixel_x_w >= pixel_w_w || // 非法尺寸或越界列报告解码错误。
                {1'b0,pixel_y_w} >= (({1'b0,pixel_h_w}+17'd15) & 17'h1fff0)) pixel_error <= 1'b1; // 超过对齐 MCU 高度的行不是合法填充。
        end // 结束像素握手检查。
        if (state_q == STATE_DRAIN && next_state_r == STATE_IDLE && !frame_pixels_complete_w) pixel_error <= 1'b1; // 缺像素帧排空后保留错误状态。
    end // 结束像素计数及完整性检查。
end

// 每十六条源图行建立一次写栅栏；水位发布时此前的写地址、写数据和写响应均已完成。
assign fence_trigger_w = !progress_fence_q && pixel_valid_w && pixel_geometry_valid_w && // 非法图像不建立源图提交水位。
                         ({1'b0,pixel_y_w} >= {6'd0,progress_next_commit_q}) &&
                         ({5'd0,progress_next_commit_q} < {1'b0,pixel_h_w});
always @(posedge clk_i) begin
    if (rst_i) begin
        progress_dst_base_q <= 0;
        progress_rows_committed_q <= 0;
        progress_next_commit_q <= 11'd16;
        progress_height_q <= 0;
        progress_pixels_expected_q <= 32'd0; // 复位后没有可报告完成的图像。
        progress_generation_q <= 0;
        progress_fence_q <= 0;
        progress_done_q <= 0;
    end else begin
        if (jpeg_ctrl_start_out_w) begin
            progress_dst_base_q <= jpeg_dst_addr_out_w;
            progress_rows_committed_q <= 0;
            progress_next_commit_q <= 11'd16;
            progress_height_q <= 0;
            progress_pixels_expected_q <= 32'd0; // 新代次必须重新取得 JPEG 图像尺寸。
            progress_generation_q <= progress_generation_q + 1'b1;
            progress_fence_q <= 0;
            progress_done_q <= 0;
        end else begin
            if (pixel_valid_w && pixel_geometry_valid_w) begin // 在核心复位之前锁存合法源图尺寸。
                progress_height_q <= pixel_h_w[10:0]; // 新输入的最终提交水位为 1328，十一位无需扩展。
                progress_pixels_expected_q <= {16'd0,pixel_w_w} * {16'd0,pixel_h_w}; // 保留三十二位图像面积用于完成检查。
            end // 结束源图尺寸锁存。
            if (fence_trigger_w) progress_fence_q <= 1;
            if (progress_fence_q && pixel_pipe_valid == 0 && burst_idle &&
                writes_pending == 0 && !outport_awvalid_o && !frame_fault_w) begin
                if (progress_next_commit_q < progress_height_q)
                    progress_rows_committed_q <= progress_next_commit_q;
                else
                    progress_rows_committed_q <= progress_height_q;
                progress_next_commit_q <= progress_next_commit_q + 11'd16;
                progress_fence_q <= 0;
            end
            if (state_q == STATE_DRAIN && next_state_r == STATE_IDLE &&
                !frame_fault_w && frame_pixels_complete_w) begin // 像素齐全且 AXI 写响应全部返回后才报告完成。
                progress_rows_committed_q <= progress_height_q;
                progress_done_q <= 1;
            end
        end
    end
end
always @(posedge clk_i) begin // START锁存帧地址，错误和取消保留至下一合法START
    if (rst_i || jpeg_ctrl_start_out_w) begin
        bus_error <= 1'b0; frame_abort_q <= 1'b0; // 新帧不能继承上次故障
        if (rst_i) frame_dst_addr_q <= 32'd0; else frame_dst_addr_q <= jpeg_dst_addr_out_w; // 忙时写配置只影响下一帧
    end else begin
        if (state_q!=STATE_IDLE && jpeg_ctrl_abort_out_w) frame_abort_q <= 1'b1; // 取消脉冲不得在排空后变成成功完成
        if (prefetch_error_w || burst_error_w || bad_bid_w) bus_error <= 1'b1; // 通道错误立即保持忙并进入排空
    end
end

//-----------------------------------------------------------------
// 状态机
//-----------------------------------------------------------------

// 当前状态

always @ *
begin
    next_state_r = state_q;

    case (state_q)
    STATE_IDLE :
    begin
        if (jpeg_ctrl_start_out_w)
            next_state_r  = STATE_FILL;
    end
    STATE_FILL:
    begin
        if ((fifo_in_level_w > (BUFFER_DEPTH/2)) || (remaining_q == 32'b0))
            next_state_r  = STATE_ACTIVE;
    end
    STATE_ACTIVE:
    begin
        if (core_busy_q && core_idle_w)
            next_state_r  = STATE_DRAIN;
    end
    STATE_DRAIN:
    begin
        if (burst_idle && pixel_pipe_valid == 0 && prefetch_idle_w)
            next_state_r = STATE_IDLE;
    end
    default:
        ;
    endcase

    if (frame_fault_w)
        next_state_r = STATE_DRAIN; // 错误/取消不能提前报告空闲
    if (state_q==STATE_DRAIN && burst_idle && pixel_pipe_valid==2'b00 && prefetch_idle_w)
        next_state_r = STATE_IDLE; // 故障粘滞但允许真实事务排空后结束
end

always @ (posedge clk_i or posedge rst_i)
if (rst_i)
    state_q <= STATE_IDLE;
else
    state_q <= next_state_r;

assign jpeg_status_busy_in_w = (state_q != STATE_IDLE);    

//-----------------------------------------------------------------
// 核心使能控制
//-----------------------------------------------------------------

always @ (posedge clk_i or posedge rst_i)
if (rst_i)
    core_active_q  <= 1'b0;
else if (state_q == STATE_IDLE && next_state_r == STATE_FILL)
    core_active_q  <= 1'b1;
else if (next_state_r == STATE_DRAIN)
    core_active_q  <= 1'b0; // FILL阶段错误也必须停止核心

//-----------------------------------------------------------------
// 核心忙状态记录
//-----------------------------------------------------------------
always @ (posedge clk_i or posedge rst_i)
if (rst_i)
    core_busy_q  <= 1'b0;
else if (state_q == STATE_IDLE || state_q == STATE_DRAIN)
    core_busy_q <= 1'b0; // 清除上一帧曾忙状态
else if (state_q == STATE_ACTIVE && !core_idle_w)
    core_busy_q  <= 1'b1;
else if (state_q == STATE_ACTIVE && core_idle_w)
    core_busy_q  <= 1'b0;

//-----------------------------------------------------------------
// 输入双缓冲预取，保持同一JPEG核心及原四字节向上取整预读边界。
//-----------------------------------------------------------------
mjpeg_jpeg_prefetch #(.AXI_ID(AXI_ID)) input_prefetch (
    .clk(clk_i), .rst(rst_i), .start(jpeg_ctrl_start_out_w), .stop(read_stop_w), // 只在旧响应完全排空后开始新帧
    .src_addr(jpeg_src_addr_out_w), .byte_length(jpeg_ctrl_length_out_w), // 输入地址和长度由队列启动时锁存
    .data_valid(fifo_jpeg_valid_w), .data_ready(jpeg_accept_w), .data(jpeg_data_w), .level(fifo_in_level_w), // 输入字序不变
    .araddr(outport_araddr_o), .arlen(outport_arlen_o), .arvalid(outport_arvalid_o), .arready(outport_arready_i), // 两个短突发在途
    .rdata(outport_rdata_i), .rresp(outport_rresp_i), .rid(outport_rid_i), .rlast(outport_rlast_i), .rvalid(outport_rvalid_i), .rready(outport_rready_o), // 真实R握手
    .idle(prefetch_idle_w), .error(prefetch_error_w), .pending_reads(prefetch_pending_w), .bytes_unissued(remaining_q) // 帧结束和错误排空依据
);
assign outport_arburst_o=2'b01; assign outport_arid_o=AXI_ID; // 原接口保持INCR及同一ID
assign jpeg_valid_w=fifo_jpeg_valid_w && state_q==STATE_ACTIVE && !frame_fault_w; // 错误不再送入JPEG核心

//-----------------------------------------------------------------
// JPEG 解码核心
//-----------------------------------------------------------------



jpeg_core #(.SUPPORT_WRITABLE_DHT(1))
u_core
(
     .clk_i(clk_i)
    ,.rst_i(~core_active_q)

    ,.inport_valid_i(jpeg_valid_w)
    ,.inport_data_i(jpeg_data_w)
    ,.inport_strb_i(4'hF)
    ,.inport_last_i(1'b0)
    ,.inport_accept_o(core_accept_w)

    ,.outport_valid_o(pixel_valid_w)
    ,.outport_width_o(pixel_w_w)
    ,.outport_height_o(pixel_h_w)
    ,.outport_pixel_x_o(pixel_x_w)
    ,.outport_pixel_y_o(pixel_y_w)
    ,.outport_pixel_r_o(pixel_r_w)
    ,.outport_pixel_g_o(pixel_g_w)
    ,.outport_pixel_b_o(pixel_b_w)
    ,.outport_accept_i(fifo_accept_in_w)

    ,.idle_o(core_idle_w)
);

assign jpeg_accept_w = core_accept_w && state_q == STATE_ACTIVE && !frame_fault_w; // 核心停止期间不消耗输入槽

// 地址与像素采用两级可停顿流水，连续有效像素可每拍进入。
assign fifo_accept_in_w = pixel_step && state_q==STATE_ACTIVE && !frame_fault_w && !progress_fence_q && !fence_trigger_w; // 只在有效帧接收新像素
always @(posedge clk_i) begin
    if (rst_i || jpeg_ctrl_start_out_w || frame_fault_w) begin
        pixel_pipe_valid <= 0;
        pixel_offset_q <= 0;
        pixel_addr_q <= 0;
        pixel_data_q <= 0;
        pixel_data_stage <= 0;
        pixel_x_stage <= 0;
    end else if (pixel_step) begin
        pixel_pipe_valid[0] <= pixel_valid_w && fifo_accept_in_w && pixel_in_frame_w; // 非法尺寸和底部 MCU 填充不进入 DDR 写流水。
        pixel_pipe_valid[1] <= pixel_pipe_valid[0];
        if (pixel_valid_w && fifo_accept_in_w && pixel_in_frame_w) begin // 只锁存完整源图中可写入的像素。
            pixel_offset_q <= pixel_w_w * pixel_y_w; // 1328 像素行按四字节 RGBX 写入，字节行距为 5312。
            pixel_x_stage <= pixel_x_w;
            pixel_data_stage <= {8'd0, pixel_b_w, pixel_g_w, pixel_r_w};
        end
        if (pixel_pipe_valid[0]) begin
            pixel_addr_q <= frame_dst_addr_q + (pixel_offset_q << 2) + {14'd0,pixel_x_stage,2'b0};
            pixel_data_q <= pixel_data_stage;
        end
    end
end

mjpeg_rgb_burst burst_writer (
    .clk(clk_i),
    .rst(rst_i), .clear(jpeg_ctrl_start_out_w), .abort(frame_fault_w), // 出错只停止新增组，已接受AW的数据不能取消

    .pixel_valid(pixel_pipe_valid[1]), .pixel_ready(burst_ready),
    .pixel_addr(pixel_addr_q), .pixel_data(pixel_data_q),

    .awvalid(outport_awvalid_o), .awready(outport_awready_i),
    .awaddr(outport_awaddr_o), .awlen(outport_awlen_o),

    .wvalid(outport_wvalid_o), .wready(outport_wready_i),
    .wdata(outport_wdata_o), .wlast(outport_wlast_o),

    .bvalid(outport_bvalid_i), .bresp(outport_bresp_i), .bready(outport_bready_o), // 完成以成功返回的B为依据
    .idle(burst_idle), .error(burst_error_w), .pending_writes(burst_pending_w)
);

assign outport_awburst_o = 2'b01;
assign outport_awid_o = AXI_ID;
assign outport_wstrb_o = 4'hf;
endmodule
