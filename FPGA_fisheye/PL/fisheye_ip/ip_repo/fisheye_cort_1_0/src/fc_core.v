module fc_core ( // 双鱼眼校正与 Q8 融合：装载通路预取下一行，计算通路消费已发布行。
    input wire clk, input wire rst_n, input wire start, input wire enable,
    input wire [31:0] src_base, input wire [31:0] x_base, input wire [31:0] y_base,
    input wire [31:0] rear_src_base, input wire [31:0] rear_x_base, input wire [31:0] rear_y_base, input wire [31:0] weight_base,
    input wire [10:0] crop_x, input wire [10:0] crop_y, input wire wait_source, // 兼容旧接口；当前完整源图模式不使用裁剪偏移。
    input wire [31:0] producer_dst_base, input wire [10:0] producer_rows, input wire [7:0] producer_generation,
    input wire producer_active, input wire producer_done,
    input wire [31:0] rear_producer_dst_base, input wire [10:0] rear_producer_rows, input wire [7:0] rear_producer_generation,
    input wire rear_producer_active, input wire rear_producer_done,
    output wire cmd_valid, input wire cmd_ready, output wire [31:0] cmd_addr, output wire [19:0] cmd_beats,
    input wire data_valid, output wire data_ready, input wire [127:0] data,
    input wire [10:0] fifo_level, output wire fifo_push, output wire [127:0] fifo_data,
    input wire stream_enable, output wire [31:0] stream_data, output wire [3:0] stream_keep,
    output wire stream_last, output wire stream_valid, input wire stream_ready,
    output reg stream_done, output wire producer_wait, output wire source_all_ready,
    output wire contract_warning, output reg contract_error
);
    localparam [11:0] MAX_CACHE_ROWS=12'd384; // 每个镜头缓存最多 384 个源行，超出范围时停止并报告契约错误。
    localparam [4:0] L_IDLE=5'd0,L_SLOT=5'd1,L_WCMD=5'd2,L_WDATA=5'd3,L_XCMD=5'd4,L_XDATA=5'd5,L_YCMD=5'd6,L_YDATA=5'd7;
    localparam [4:0] L_BOUND=5'd8,L_PROBE=5'd9,L_SCMD=5'd10,L_SDATA=5'd11,L_ADV=5'd12,L_COMMIT=5'd13,L_WFIN=5'd14,L_NEXT=5'd15,L_DRAIN=5'd16;
    reg [4:0] load_state; reg [10:0] load_row,source_row; reg load_bank,source_lens;
    reg [8:0] weight_beat; reg [9:0] coord_group,interval_index,interval_first,interval_limit; reg coord_lens; // 贡献区间以四像素组为单位，使用半开区间 [start,end)。
    reg [9:0] interval_count_f,interval_count_r; reg interval_open_f,interval_open_r;
    (* ram_style="distributed" *) reg [9:0] interval_start_f [0:331],interval_end_f [0:331];
    (* ram_style="distributed" *) reg [9:0] interval_start_r [0:331],interval_end_r [0:331];
    reg [10:0] need_min_f,need_max_f,need_min_r,need_max_r; reg need_f,need_r,mapping_bad;
    reg [1:0] row_ready,row_uses_f,row_uses_r; // 两个输出行槽发布后，保护其引用的源行直到计算流水排空。
    reg [10:0] row_min_f [0:1],row_max_f [0:1],row_min_r [0:1],row_max_r [0:1];
    (* ram_style="block" *) reg [127:0] scratch_x [0:663]; // 前后镜头复用 X 坐标暂存；收到 Y 后再打包写入对应 LUT。
    (* ram_style="ultra" *) reg [135:0] lut_front [0:1327]; // 每字容纳四组 {Y[16:0],X[16:0]}；两行共 1328 字。
    (* ram_style="ultra" *) reg [135:0] lut_rear [0:1327];
    (* ram_style="block" *) reg [127:0] weights [0:663];
    reg [135:0] front_q0,front_q1,rear_q0,rear_q1; reg [127:0] weight_q0,weight_q1;
    reg [2:0] lane_q0,lane_q1;
    reg compute_busy,compute_bank,issue_active; reg [10:0] compute_row; reg [11:0] issue_x;
    reg [13:0] pipe_valid,pipe_last; // 14 级：LUT 读取 2 拍、源缓存 6 拍、双线性插值 4 拍、融合 2 拍。
    wire output_capacity,ce,issue,row_finish;
    assign output_capacity=fifo_level<11'd500;
    assign ce=enable && !start && output_capacity && (!stream_enable || !pipe_valid[13] || stream_ready); // 公共 CE 同步冻结数据、有效位和元数据，使 FIFO/AXIS 背压保持对齐。
    assign issue=compute_busy && issue_active;
    assign row_finish=ce && pipe_valid[13] && pipe_last[13];
    wire [10:0] lut_rd_addr; wire [9:0] weight_rd_addr,weight_wr_addr;
    assign lut_rd_addr=(compute_bank ? 11'd664 : 11'd0)+{1'b0,issue_x[11:2]};
    assign weight_rd_addr=(compute_bank ? 10'd332 : 10'd0)+{1'b0,issue_x[11:3]};
    assign weight_wr_addr=(load_bank ? 10'd332 : 10'd0)+{1'b0,weight_beat};
    wire [33:0] pixel_pair_f,pixel_pair_r; wire [15:0] pixel_weight;
    assign pixel_pair_f=pixel_weight!=16'd0 ? front_q1>>(lane_q1[1:0]*7'd34) : 34'd0; assign pixel_pair_r=pixel_weight<16'd256 ? rear_q1>>(lane_q1[1:0]*7'd34) : 34'd0;
    assign pixel_weight=weight_q1>>{lane_q1,4'b0}; // uint16 Q8 前向权重：0 仅取后向，256 仅取前向。
    wire weight_take; wire [9:0] weight_group0,weight_group1; wire [7:0] weight_front_active,weight_rear_active;
    assign weight_take=enable && !start && load_state==L_WDATA && data_valid && data_ready;
    assign weight_group0={weight_beat,1'b0}; assign weight_group1={weight_beat,1'b1};
    genvar wi;
    generate for(wi=0;wi<8;wi=wi+1) begin: gen_weight // 权重装载时生成贡献区间，只读取包含活动像素的整组 X/Y 坐标。
        assign weight_front_active[wi]=data[wi*16+:16]!=16'd0;
        assign weight_rear_active[wi]=data[wi*16+:16]<16'd256;
    end endgenerate
    wire [1:0] group_f,group_r;
    assign group_f={|weight_front_active[7:4],|weight_front_active[3:0]};
    assign group_r={|weight_rear_active[7:4],|weight_rear_active[3:0]};
    always @(posedge clk) begin
        if(weight_take) begin
            if((!interval_open_f && group_f[0]) || (!group_f[0] && group_f[1])) interval_start_f[interval_count_f]<=group_f[0] ? weight_group0 : weight_group1;
            if((interval_open_f && !group_f[0]) || (group_f[0] && !group_f[1])) interval_end_f[(!interval_open_f && group_f[0]) ? interval_count_f : interval_count_f-1'b1]<=interval_open_f && !group_f[0] ? weight_group0 : weight_group1;
        end
        if(enable && !start && load_state==L_WFIN && interval_open_f) interval_end_f[interval_count_f-1'b1]<=10'd664; // 尾段在独立周期闭合，避免与权重接收共用写端口。
    end
    always @(posedge clk) begin
        if(weight_take) begin
            if((!interval_open_r && group_r[0]) || (!group_r[0] && group_r[1])) interval_start_r[interval_count_r]<=group_r[0] ? weight_group0 : weight_group1;
            if((interval_open_r && !group_r[0]) || (group_r[0] && !group_r[1])) interval_end_r[(!interval_open_r && group_r[0]) ? interval_count_r : interval_count_r-1'b1]<=interval_open_r && !group_r[0] ? weight_group0 : weight_group1;
        end
        if(enable && !start && load_state==L_WFIN && interval_open_r) interval_end_r[interval_count_r-1'b1]<=10'd664;
    end
    wire [9:0] selected_interval_start,selected_interval_end,selected_interval_count;
    assign selected_interval_start=coord_lens ? interval_start_r[interval_index] : interval_start_f[interval_index];
    assign selected_interval_end=coord_lens ? interval_end_r[interval_index] : interval_end_f[interval_index];
    assign selected_interval_count=coord_lens ? interval_count_r : interval_count_f;
    wire coordinate_take; reg pair_valid,pair_lens,pair_half; reg [10:0] pair_addr;
    reg [127:0] pair_x,pair_y,pair_weight; wire [63:0] pair_weights4;
    wire [135:0] packed_pair; wire pair_bad,pair_used; wire [10:0] pair_min,pair_max;
    assign coordinate_take=enable && !start && load_state==L_YDATA && data_valid && data_ready;
    assign pair_weights4=pair_weight>>(pair_half ? 7'd64 : 7'd0);
    fc_coord_pack4 u_pack4 ( // 先检查完整 int32 坐标，再无损压缩为 17 位，防止截断掩盖越界。
        .x_word(pair_x),.y_word(pair_y),.weights4(pair_weights4),.rear(pair_lens),
        .packed_word(packed_pair),.bad(pair_bad),.used(pair_used),.min_row(pair_min),.max_row(pair_max)
    );
    always @(posedge clk) begin // RAM 阵列不复位；由有效位和行发布状态屏蔽未初始化内容。
        if(!rst_n || start) pair_valid<=0; else pair_valid<=coordinate_take; // 最后一拍 Y 返回后，必须等待配对写入及源行范围归约排空。
        if(coordinate_take) begin
            pair_x<=scratch_x[coord_group]; pair_y<=data; pair_weight<=weights[(load_bank ? 10'd332 : 10'd0)+{1'b0,coord_group[9:1]}];
            pair_lens<=coord_lens; pair_half<=coord_group[0]; pair_addr<=(load_bank ? 11'd664 : 11'd0)+{1'b0,coord_group};
        end
        if(enable && !start && load_state==L_XDATA && data_valid && data_ready) scratch_x[coord_group]<=data;
        if(pair_valid && enable && !start) begin
            if(pair_lens) lut_rear[pair_addr]<=packed_pair; else lut_front[pair_addr]<=packed_pair;
        end
        if(weight_take) weights[weight_wr_addr]<=data;
        if(ce) begin
            front_q0<=lut_front[lut_rd_addr]; front_q1<=front_q0; rear_q0<=lut_rear[lut_rd_addr]; rear_q1<=rear_q0;
            weight_q0<=weights[weight_rd_addr]; weight_q1<=weight_q0; lane_q0<=issue_x[2:0]; lane_q1<=lane_q0;
        end
    end
    reg seen_f, seen_r, ready_f, ready_r; reg [7:0] generation_f, generation_r; // 前后镜头分别锁存代次，不要求两个解码器的计数值相等。
    wire identity_f, identity_r, source_available;
    assign identity_f = seen_f && producer_dst_base == src_base && producer_generation == generation_f;
    assign identity_r = seen_r && rear_producer_dst_base == rear_src_base && rear_producer_generation == generation_r;
    assign source_available = !wait_source || (source_lens ? (ready_r || (identity_r && rear_producer_rows > source_row)) : (ready_f || (identity_f && producer_rows > source_row))); // 按源帧身份与已提交行水位读取；整帧就绪后保留该状态。
    assign source_all_ready = ready_f && ready_r;
    assign producer_wait = enable && !start && (load_state == L_PROBE || load_state == L_SCMD) && !source_available;
    assign contract_warning = 1'b0;
    wire hit_f, hit_r, safe_f, safe_r, fill_ready_f, fill_ready_r, fill_busy_f, fill_busy_r, fill_done_f, fill_done_r;
    wire selected_hit, selected_safe, selected_done, selected_fill_ready;
    assign selected_hit = source_lens ? hit_r : hit_f; assign selected_safe = source_lens ? safe_r : safe_f;
    assign selected_done = source_lens ? fill_done_r : fill_done_f; assign selected_fill_ready = source_lens ? fill_ready_r : fill_ready_f;
    wire fill_start_f, fill_start_r;
    assign fill_start_f = cmd_valid && cmd_ready && load_state == L_SCMD && !source_lens;
    assign fill_start_r = cmd_valid && cmd_ready && load_state == L_SCMD && source_lens;
    wire [31:0] coordinate_base;
    assign coordinate_base=load_state==L_XCMD ? (coord_lens ? rear_x_base : x_base) : (coord_lens ? rear_y_base : y_base);
    assign cmd_valid=enable && !start && (load_state==L_WCMD || load_state==L_XCMD || load_state==L_YCMD || (load_state==L_SCMD && source_available && selected_safe));
    assign cmd_addr=load_state==L_WCMD ? weight_base+{21'd0,load_row}*32'd5312 : load_state==L_SCMD ? (source_lens ? rear_src_base : src_base)+{21'd0,source_row}*32'd5312 : coordinate_base+{21'd0,load_row}*32'd10624+{18'd0,interval_first,4'd0}; // 字节步长：源 RGBX 行 5312，输出权重行 5312，int32 坐标行 10624。
    assign cmd_beats=load_state==L_WCMD || load_state==L_SCMD ? 20'd332 : {10'd0,interval_limit}-{10'd0,interval_first};
    assign data_ready=enable && !start && (load_state==L_WDATA || load_state==L_XDATA || load_state==L_YDATA || (load_state==L_SDATA && selected_fill_ready));
    always @(posedge clk) begin
        if(!rst_n || start) begin
            if(start) load_state<=L_SLOT;else load_state<=L_IDLE; load_row<=0; load_bank<=0; source_row<=0; source_lens<=0;
            weight_beat<=0; coord_group<=0; interval_index<=0; interval_first<=0; interval_limit<=0; coord_lens<=0;
            interval_count_f<=0; interval_count_r<=0; interval_open_f<=0; interval_open_r<=0;
            need_f<=0; need_r<=0; mapping_bad<=0; need_min_f<=11'd1327; need_min_r<=11'd1327; need_max_f<=0; need_max_r<=0;
            row_ready<=0; row_uses_f<=0; row_uses_r<=0; contract_error<=0;
            seen_f<=0; seen_r<=0; ready_f<=start && !wait_source; ready_r<=start && !wait_source; generation_f<=0; generation_r<=0;
            row_min_f[0]<=0; row_max_f[0]<=0; row_min_r[0]<=0; row_max_r[0]<=0;
            row_min_f[1]<=0; row_max_f[1]<=0; row_min_r[1]<=0; row_max_r[1]<=0;
        end else if(enable) begin
            if(row_finish) row_ready[compute_bank]<=0; // 只有末像素被接受后，才允许装载通路复用该行槽及其源缓存。
            if (wait_source && !ready_f) begin
                if (!seen_f && producer_dst_base == src_base && (producer_active || producer_done)) begin seen_f <= 1; generation_f <= producer_generation; if (producer_rows >= 11'd1328) ready_f <= 1; end
                else if (identity_f && producer_rows >= 11'd1328) ready_f <= 1;
                else if (seen_f && !identity_f && (producer_active || producer_done)) contract_error <= 1;
            end
            if (wait_source && !ready_r) begin
                if (!seen_r && rear_producer_dst_base == rear_src_base && (rear_producer_active || rear_producer_done)) begin seen_r <= 1; generation_r <= rear_producer_generation; if (rear_producer_rows >= 11'd1328) ready_r <= 1; end
                else if (identity_r && rear_producer_rows >= 11'd1328) ready_r <= 1;
                else if (seen_r && !identity_r && (rear_producer_active || rear_producer_done)) contract_error <= 1;
            end
            case(load_state)
                L_SLOT: if(!row_ready[load_bank]) begin
                    need_f<=0; need_r<=0; mapping_bad<=0; need_min_f<=11'd1327; need_min_r<=11'd1327; need_max_f<=0; need_max_r<=0;
                    weight_beat<=0; interval_count_f<=0; interval_count_r<=0; interval_open_f<=0; interval_open_r<=0; load_state<=L_WCMD;
                end
                L_WCMD: if(cmd_valid && cmd_ready) begin weight_beat<=0; load_state<=L_WDATA; end
                L_WDATA: if(weight_take) begin
                    if(data[15:0]>16'd256 || data[31:16]>16'd256 || data[47:32]>16'd256 || data[63:48]>16'd256 || data[79:64]>16'd256 || data[95:80]>16'd256 || data[111:96]>16'd256 || data[127:112]>16'd256) mapping_bad<=1; // 完整检查八个 uint16 权重均在 0..256 内，之后才使用低九位。
                    if((!interval_open_f && group_f[0]) || (!group_f[0] && group_f[1])) interval_count_f<=interval_count_f+1'b1;
                    interval_open_f<=group_f[1];
                    if((!interval_open_r && group_r[0]) || (!group_r[0] && group_r[1])) interval_count_r<=interval_count_r+1'b1;
                    interval_open_r<=group_r[1];
                    if(weight_beat==9'd331) load_state<=L_WFIN; else weight_beat<=weight_beat+1'b1;
                end
                L_WFIN: begin
                    if(mapping_bad || (interval_count_f==0 && interval_count_r==0)) begin contract_error<=1; load_state<=L_IDLE; end
                    else begin coord_lens<=interval_count_f==0; interval_index<=0; load_state<=L_NEXT; end
                end
                L_NEXT: begin
                    interval_first<=selected_interval_start; interval_limit<=selected_interval_end; coord_group<=selected_interval_start; load_state<=L_XCMD;
                end
                L_XCMD: if(cmd_valid && cmd_ready) begin coord_group<=interval_first; load_state<=L_XDATA; end
                L_XDATA: if(data_valid && data_ready) begin
                    if(coord_group==interval_limit-1'b1) begin coord_group<=interval_first; load_state<=L_YCMD; end else coord_group<=coord_group+1'b1;
                end
                L_YCMD: if(cmd_valid && cmd_ready) begin coord_group<=interval_first; load_state<=L_YDATA; end
                L_YDATA: if(coordinate_take) begin
                    if(coord_group==interval_limit-1'b1) load_state<=L_DRAIN; else coord_group<=coord_group+1'b1;
                end
                L_DRAIN: if(!pair_valid) begin // 各贡献区间末尾先排空坐标配对流水，再切换镜头或检查源行跨度。
                    if(interval_index+10'd1<selected_interval_count) begin interval_index<=interval_index+1'b1; load_state<=L_NEXT; end
                    else if(!coord_lens && interval_count_r!=0) begin coord_lens<=1; interval_index<=0; load_state<=L_NEXT; end
                    else load_state<=L_BOUND;
                end
                L_BOUND: begin // 各镜头所需源行跨度必须不大于 384；允许回访较早的源行。
                    if (mapping_bad || (!need_f && !need_r) || (need_f && ({1'b0,need_max_f}-{1'b0,need_min_f}+12'd1 > MAX_CACHE_ROWS || need_max_f >= 11'd1328 || need_min_f > need_max_f)) || (need_r && ({1'b0,need_max_r}-{1'b0,need_min_r}+12'd1 > MAX_CACHE_ROWS || need_max_r >= 11'd1328 || need_min_r > need_max_r))) begin contract_error <= 1; load_state <= L_IDLE; end
                    else begin source_lens <= !need_f; source_row <= need_f ? need_min_f : need_min_r; load_state <= L_PROBE; end
                end
                L_PROBE: begin
                    if (selected_hit) begin
                        if (source_row == (source_lens ? need_max_r : need_max_f)) begin
                            if (!source_lens && need_r) begin source_lens <= 1; source_row <= need_min_r; end
                            else load_state <= L_COMMIT;
                        end else source_row <= source_row+1'b1;
                    end
                    else if (selected_safe && source_available) load_state <= L_SCMD; // 未命中时同时检查覆盖保护和解码水位，避免覆盖仍在使用的源行。
                end
                L_SCMD: if (cmd_valid && cmd_ready) load_state <= L_SDATA;
                L_SDATA: if (selected_done) load_state <= L_ADV;
                L_ADV: begin
                    if (source_row == (source_lens ? need_max_r : need_max_f)) begin
                        if (!source_lens && need_r) begin source_lens <= 1; source_row <= need_min_r; load_state <= L_PROBE; end
                        else load_state <= L_COMMIT;
                    end else begin source_row <= source_row+1'b1; load_state <= L_PROBE; end
                end
                L_COMMIT: begin // 坐标、权重和所需源行全部就绪后，一次发布行槽及保护范围。
                    row_min_f[load_bank] <= need_min_f; row_max_f[load_bank] <= need_max_f; row_min_r[load_bank] <= need_min_r; row_max_r[load_bank] <= need_max_r; row_uses_f[load_bank] <= need_f; row_uses_r[load_bank] <= need_r; row_ready[load_bank] <= 1;
                    load_bank <= !load_bank; if (load_row == 11'd1327) load_state <= L_IDLE; else begin load_row <= load_row+1'b1; load_state <= L_SLOT; end
                end
                default: load_state <= L_IDLE;
            endcase
            if(pair_valid) begin
                if(pair_bad) mapping_bad<=1;
                if(pair_used) begin
                    if(pair_lens) begin
                        need_r<=1; if(!need_r || pair_min<need_min_r) need_min_r<=pair_min; if(!need_r || pair_max>need_max_r) need_max_r<=pair_max;
                    end else begin
                        need_f<=1; if(!need_f || pair_min<need_min_f) need_min_f<=pair_min; if(!need_f || pair_max>need_max_f) need_max_f<=pair_max;
                    end
                end
            end
        end
    end
    wire [23:0] pf0, pf1, pf2, pf3, rgb_f; wire [5:0] fx_f, fy_f;
    fc_lens_cache u_cache_f (
        .clk(clk), .rst_n(rst_n), .clear(start), .enable(enable),
        .probe_row(source_row), .probe_hit(hit_f), .probe_safe(safe_f),
        .protect0(row_ready[0] && row_uses_f[0]), .min0(row_min_f[0]), .max0(row_max_f[0]),
        .protect1(row_ready[1] && row_uses_f[1]), .min1(row_min_f[1]), .max1(row_max_f[1]),
        .fill_start(fill_start_f), .fill_row(source_row), .fill_data(data),
        .fill_valid(data_valid && load_state == L_SDATA && !source_lens), .fill_ready(fill_ready_f), .fill_busy(fill_busy_f), .fill_done(fill_done_f),
        .sample_ce(ce), .sample_active(pipe_valid[1] && pixel_weight != 0),
        .sample_x({15'd0,pixel_pair_f[16:0]}), .sample_y({15'd0,pixel_pair_f[33:17]}),
        .p0(pf0), .p1(pf1), .p2(pf2), .p3(pf3), .fx(fx_f), .fy(fy_f)
    );
    fc_bilinear u_bilinear_f (
        .clk(clk), .rst_n(rst_n), .clear(start), .ce(ce),
        .p0(pf0), .p1(pf1), .p2(pf2), .p3(pf3), .fx(fx_f), .fy(fy_f), .rgb(rgb_f)
    );
    wire [23:0] pr0, pr1, pr2, pr3, rgb_r; wire [5:0] fx_r, fy_r;
    fc_lens_cache u_cache_r (
        .clk(clk), .rst_n(rst_n), .clear(start), .enable(enable),
        .probe_row(source_row), .probe_hit(hit_r), .probe_safe(safe_r),
        .protect0(row_ready[0] && row_uses_r[0]), .min0(row_min_r[0]), .max0(row_max_r[0]),
        .protect1(row_ready[1] && row_uses_r[1]), .min1(row_min_r[1]), .max1(row_max_r[1]),
        .fill_start(fill_start_r), .fill_row(source_row), .fill_data(data),
        .fill_valid(data_valid && load_state == L_SDATA && source_lens), .fill_ready(fill_ready_r), .fill_busy(fill_busy_r), .fill_done(fill_done_r),
        .sample_ce(ce), .sample_active(pipe_valid[1] && pixel_weight < 16'd256),
        .sample_x({15'd0,pixel_pair_r[16:0]}), .sample_y({15'd0,pixel_pair_r[33:17]}),
        .p0(pr0), .p1(pr1), .p2(pr2), .p3(pr3), .fx(fx_r), .fy(fy_r)
    );
    fc_bilinear u_bilinear_r (
        .clk(clk), .rst_n(rst_n), .clear(start), .ce(ce),
        .p0(pr0), .p1(pr1), .p2(pr2), .p3(pr3), .fx(fx_r), .fy(fy_r), .rgb(rgb_r)
    );
    reg [8:0] weight_pipe [0:9]; // 权重延迟与源缓存、插值结果对齐，随公共 CE 推进。
    always @(posedge clk) begin
        if (!rst_n) begin
            weight_pipe[0] <= 0;
            weight_pipe[1] <= 0;
            weight_pipe[2] <= 0;
            weight_pipe[3] <= 0;
            weight_pipe[4] <= 0;
            weight_pipe[5] <= 0;
            weight_pipe[6] <= 0;
            weight_pipe[7] <= 0;
            weight_pipe[8] <= 0;
            weight_pipe[9] <= 0;
        end else if (start) begin

            weight_pipe[0] <= 0;
            weight_pipe[1] <= 0;
            weight_pipe[2] <= 0;
            weight_pipe[3] <= 0;
            weight_pipe[4] <= 0;
            weight_pipe[5] <= 0;
            weight_pipe[6] <= 0;
            weight_pipe[7] <= 0;
            weight_pipe[8] <= 0;
            weight_pipe[9] <= 0;
        end else if (ce) begin
            weight_pipe[0] <= pipe_valid[1] ? pixel_weight[8:0] : 9'd0;
            weight_pipe[1] <= weight_pipe[0];
            weight_pipe[2] <= weight_pipe[1];
            weight_pipe[3] <= weight_pipe[2];
            weight_pipe[4] <= weight_pipe[3];
            weight_pipe[5] <= weight_pipe[4];
            weight_pipe[6] <= weight_pipe[5];
            weight_pipe[7] <= weight_pipe[6];
            weight_pipe[8] <= weight_pipe[7];
            weight_pipe[9] <= weight_pipe[8];
        end
    end
    reg [16:0] fuse_rf, fuse_rr; reg [17:0] fuse_rsum; // RGB 三通道独立融合，结果右移 8 位截断，保持 Q8 运算规则。
    always @(posedge clk) begin
        if (!rst_n) begin fuse_rf <= 0; fuse_rr <= 0; fuse_rsum <= 0; end else if (start) begin fuse_rf <= 0; fuse_rr <= 0; fuse_rsum <= 0; end
        else if (ce) begin
            fuse_rf <= rgb_f[7:0]*weight_pipe[9]; fuse_rr <= rgb_r[7:0]*(9'd256-weight_pipe[9]);
            fuse_rsum <= {1'b0,fuse_rf}+{1'b0,fuse_rr};
        end
    end
    reg [16:0] fuse_gf, fuse_gr; reg [17:0] fuse_gsum;
    always @(posedge clk) begin
        if (!rst_n) begin fuse_gf <= 0; fuse_gr <= 0; fuse_gsum <= 0; end else if (start) begin fuse_gf <= 0; fuse_gr <= 0; fuse_gsum <= 0; end
        else if (ce) begin
            fuse_gf <= rgb_f[15:8]*weight_pipe[9]; fuse_gr <= rgb_r[15:8]*(9'd256-weight_pipe[9]);
            fuse_gsum <= {1'b0,fuse_gf}+{1'b0,fuse_gr};
        end
    end
    reg [16:0] fuse_bf, fuse_br; reg [17:0] fuse_bsum;
    always @(posedge clk) begin
        if (!rst_n) begin fuse_bf <= 0; fuse_br <= 0; fuse_bsum <= 0; end else if (start) begin fuse_bf <= 0; fuse_br <= 0; fuse_bsum <= 0; end
        else if (ce) begin
            fuse_bf <= rgb_f[23:16]*weight_pipe[9]; fuse_br <= rgb_r[23:16]*(9'd256-weight_pipe[9]);
            fuse_bsum <= {1'b0,fuse_bf}+{1'b0,fuse_br};
        end
    end
    wire [31:0] output_rgbx; reg [95:0] pack_data; reg [1:0] pack_lane; // RGBX 低字节按 R/G/B/X 排列，每个 128 位 DDR 数据拍容纳四个像素。
    assign output_rgbx = {8'd0,fuse_bsum[15:8],fuse_gsum[15:8],fuse_rsum[15:8]};
    assign fifo_push = ce && pipe_valid[13] && pack_lane == 2'd3;
    assign fifo_data = {output_rgbx,pack_data};
    assign stream_data = output_rgbx; assign stream_keep = 4'hf;
    assign stream_valid = enable && !start && stream_enable && pipe_valid[13] && output_capacity;
    assign stream_last = pipe_last[13] && compute_row == 11'd1327;
    always @(posedge clk) begin
        if (!rst_n) begin
            compute_busy <= 0; compute_bank <= 0; issue_active <= 0; compute_row <= 0; issue_x <= 0; pipe_valid <= 0; pipe_last <= 0; pack_lane <= 0; pack_data <= 0; stream_done <= 0;
        end else if (start) begin

            compute_busy <= 0; compute_bank <= 0; issue_active <= 0; compute_row <= 0; issue_x <= 0; pipe_valid <= 0; pipe_last <= 0; pack_lane <= 0; pack_data <= 0; stream_done <= 0;
        end else if (ce) begin
            pipe_valid <= {pipe_valid[12:0],issue}; pipe_last <= {pipe_last[12:0],issue && issue_x == 12'd2655};
            if (!compute_busy && compute_row < 11'd1328 && row_ready[compute_bank]) begin compute_busy <= 1; issue_active <= 1; issue_x <= 0; end // 只从已发布的行槽发射像素；行尾排空后交换计算槽。
            if (issue) begin if (issue_x == 12'd2655) issue_active <= 0; else issue_x <= issue_x+1'b1; end
            if (pipe_valid[13]) begin
                if (pack_lane == 0) pack_data[31:0] <= output_rgbx;
                if (pack_lane == 1) pack_data[63:32] <= output_rgbx;
                if (pack_lane == 2) pack_data[95:64] <= output_rgbx;
                pack_lane <= pack_lane+1'b1;
            end
            if (row_finish) begin
                compute_busy <= 0; compute_bank <= !compute_bank; compute_row <= compute_row+1'b1;
                if (compute_row == 11'd1327) stream_done <= 1;
            end
        end
    end
endmodule

module fc_lens_cache ( // 单镜头源行缓存：行标签复用 384 个槽，奇偶行列分成四个存储体。
    input wire clk, input wire rst_n, input wire clear, input wire enable,
    input wire [10:0] probe_row, output wire probe_hit, output wire probe_safe,
    input wire protect0, input wire [10:0] min0, input wire [10:0] max0,
    input wire protect1, input wire [10:0] min1, input wire [10:0] max1,
    input wire fill_start, input wire [10:0] fill_row, input wire [127:0] fill_data,
    input wire fill_valid, output wire fill_ready, output reg fill_busy, output reg fill_done,
    input wire sample_ce, input wire sample_active, input wire [31:0] sample_x, input wire [31:0] sample_y,
    output wire [23:0] p0, output wire [23:0] p1, output wire [23:0] p2, output wire [23:0] p3,
    output wire [5:0] fx, output wire [5:0] fy
);
    (* ram_style="ultra" *) reg [71:0] bank00 [0:42623]; // 每个 72 位字打包三个 RGB888 像素，去除 RGBX 的填充字节。
    (* ram_style="ultra" *) reg [71:0] bank01 [0:42623];
    (* ram_style="ultra" *) reg [71:0] bank10 [0:42623];
    (* ram_style="ultra" *) reg [71:0] bank11 [0:42623];
    reg [383:0] valid_rows; // 槽号为源行模 384；标签区分映射到同槽的不同源行。
    reg [1:0] tags [0:383];
    wire [8:0] probe_slot;
    wire [1:0] probe_page;
    assign probe_slot = probe_row >= 11'd1152 ? probe_row-11'd1152 : probe_row >= 11'd768 ? probe_row-11'd768 : probe_row >= 11'd384 ? probe_row-11'd384 : probe_row;
    assign probe_page = probe_row >= 11'd1152 ? 2'd3 : probe_row >= 11'd768 ? 2'd2 : probe_row >= 11'd384 ? 2'd1 : 2'd0;
    wire [10:0] resident_row;
    assign resident_row = {9'd0,tags[probe_slot]}*11'd384+{2'd0,probe_slot};
    assign probe_hit = probe_row < 11'd1328 && valid_rows[probe_slot] && tags[probe_slot] == probe_page;
    assign probe_safe = !valid_rows[probe_slot] || (!(protect0 && resident_row >= min0 && resident_row <= max0) && !(protect1 && resident_row >= min1 && resident_row <= max1)); // 两个已发布输出行引用的驻留源行均禁止覆盖。
    wire [8:0] fill_slot;
    wire [1:0] fill_page;
    assign fill_slot = fill_row >= 11'd1152 ? fill_row-11'd1152 : fill_row >= 11'd768 ? fill_row-11'd768 : fill_row >= 11'd384 ? fill_row-11'd384 : fill_row;
    assign fill_page = fill_row >= 11'd1152 ? 2'd3 : fill_row >= 11'd768 ? 2'd2 : fill_row >= 11'd384 ? 2'd1 : 2'd0;
    reg [8:0] active_slot; reg [1:0] active_page; reg row_parity;
    reg [15:0] fill_base; reg [8:0] word_index, beat_count;
    reg [47:0] carry_even, carry_odd; reg [1:0] carry_count; reg tail;
    wire [15:0] write_addr;
    assign write_addr = fill_base+{7'd0,word_index};
    assign fill_ready = enable && fill_busy && !tail && !clear;
    always @(posedge clk) begin
        if (!rst_n) begin
            valid_rows <= 384'd0; fill_busy <= 0; fill_done <= 0; active_slot <= 0; active_page <= 0; row_parity <= 0; fill_base <= 0; word_index <= 0; beat_count <= 0; carry_even <= 0; carry_odd <= 0; carry_count <= 0; tail <= 0;
        end else if (clear) begin

            valid_rows <= 384'd0; fill_busy <= 0; fill_done <= 0; active_slot <= 0; active_page <= 0; row_parity <= 0; fill_base <= 0; word_index <= 0; beat_count <= 0; carry_even <= 0; carry_odd <= 0; carry_count <= 0; tail <= 0;
        end else begin
            fill_done <= 0;
            if (enable && fill_start && !fill_busy) begin
                active_slot <= fill_slot; active_page <= fill_page; row_parity <= fill_row[0]; fill_base <= {8'd0,fill_slot[8:1]}*16'd222;
                valid_rows[fill_slot] <= 0; fill_busy <= 1; word_index <= 0; beat_count <= 0; carry_count <= 0; tail <= 0; // 覆盖开始即撤销旧有效位，整行写完后才重新发布标签。
            end else if (enable && fill_busy && tail) begin // 补写尾部不足三个像素的字后，再提交整行有效状态。
                tags[active_slot] <= active_page; valid_rows[active_slot] <= 1; fill_busy <= 0; fill_done <= 1; tail <= 0;
            end else if (fill_valid && fill_ready) begin
                beat_count <= beat_count+1'b1;
                case (carry_count)
                    2'd0: begin carry_even <= {fill_data[87:64],fill_data[23:0]}; carry_odd <= {fill_data[119:96],fill_data[55:32]}; carry_count <= 2; end
                    2'd2: begin carry_even <= {24'd0,fill_data[87:64]}; carry_odd <= {24'd0,fill_data[119:96]}; carry_count <= 1; word_index <= word_index+1'b1; end
                    default: begin carry_count <= 0; word_index <= word_index+1'b1; end
                endcase
                if (beat_count == 9'd331) tail <= 1;
            end
        end
    end
    wire write_word, write_tail; wire [71:0] write_even, write_odd; // 每次接收四个 RGBX，按奇偶列重组为三像素字；尾字其余位补零。
    assign write_word = fill_valid && fill_ready && carry_count != 0;
    assign write_tail = enable && fill_busy && tail && !clear;
    assign write_even = tail ? {48'd0,carry_even[23:0]} : carry_count == 2 ? {fill_data[23:0],carry_even} : {fill_data[87:64],fill_data[23:0],carry_even[23:0]};
    assign write_odd = tail ? {48'd0,carry_odd[23:0]} : carry_count == 2 ? {fill_data[55:32],carry_odd} : {fill_data[119:96],fill_data[55:32],carry_odd[23:0]};
    always @(posedge clk) begin
        if (rst_n && (write_word || write_tail)) begin
            if (!row_parity) begin bank00[write_addr] <= write_even; bank01[write_addr] <= write_odd; end
            else begin bank10[write_addr] <= write_even; bank11[write_addr] <= write_odd; end
        end
    end
    wire [10:0] sx, sy, ex, ox, ey, oy;
    assign sx = sample_active ? sample_x[16:6] : 11'd0; assign sy = sample_active ? sample_y[16:6] : 11'd0;
    assign ex = sx[0] ? sx+1'b1 : sx; assign ox = sx[0] ? sx : sx+1'b1;
    assign ey = sy[0] ? sy+1'b1 : sy; assign oy = sy[0] ? sy : sy+1'b1;
    wire [8:0] even_slot, odd_slot;
    assign even_slot = ey >= 11'd1152 ? ey-11'd1152 : ey >= 11'd768 ? ey-11'd768 : ey >= 11'd384 ? ey-11'd384 : ey;
    assign odd_slot = oy >= 11'd1152 ? oy-11'd1152 : oy >= 11'd768 ? oy-11'd768 : oy >= 11'd384 ? oy-11'd384 : oy;
    reg [7:0] even_half, odd_half; reg [7:0] even_word, odd_word;
    reg [15:0] addr00, addr01, addr10, addr11;
    reg [5:0] active_pipe, xp_pipe, yp_pipe; // 六拍缓存读流水同步传递分数坐标、奇偶选择和字内像素编号。
    reg [5:0] fx_pipe [0:5]; reg [5:0] fy_pipe [0:5];
    reg [1:0] even_lane [0:5]; reg [1:0] odd_lane [0:5];
    always @(posedge clk) begin
        if (!rst_n) begin
            active_pipe <= 0; xp_pipe <= 0; yp_pipe <= 0; even_half <= 0; odd_half <= 0; even_word <= 0; odd_word <= 0; addr00 <= 0; addr01 <= 0; addr10 <= 0; addr11 <= 0;
            fx_pipe[0] <= 0; fy_pipe[0] <= 0; even_lane[0] <= 0; odd_lane[0] <= 0;
            fx_pipe[1] <= 0; fy_pipe[1] <= 0; even_lane[1] <= 0; odd_lane[1] <= 0;
            fx_pipe[2] <= 0; fy_pipe[2] <= 0; even_lane[2] <= 0; odd_lane[2] <= 0;
            fx_pipe[3] <= 0; fy_pipe[3] <= 0; even_lane[3] <= 0; odd_lane[3] <= 0;
            fx_pipe[4] <= 0; fy_pipe[4] <= 0; even_lane[4] <= 0; odd_lane[4] <= 0;
            fx_pipe[5] <= 0; fy_pipe[5] <= 0; even_lane[5] <= 0; odd_lane[5] <= 0;
        end else if (clear) begin

            active_pipe <= 0; xp_pipe <= 0; yp_pipe <= 0; even_half <= 0; odd_half <= 0; even_word <= 0; odd_word <= 0; addr00 <= 0; addr01 <= 0; addr10 <= 0; addr11 <= 0;
            fx_pipe[0] <= 0; fy_pipe[0] <= 0; even_lane[0] <= 0; odd_lane[0] <= 0;
            fx_pipe[1] <= 0; fy_pipe[1] <= 0; even_lane[1] <= 0; odd_lane[1] <= 0;
            fx_pipe[2] <= 0; fy_pipe[2] <= 0; even_lane[2] <= 0; odd_lane[2] <= 0;
            fx_pipe[3] <= 0; fy_pipe[3] <= 0; even_lane[3] <= 0; odd_lane[3] <= 0;
            fx_pipe[4] <= 0; fy_pipe[4] <= 0; even_lane[4] <= 0; odd_lane[4] <= 0;
            fx_pipe[5] <= 0; fy_pipe[5] <= 0; even_lane[5] <= 0; odd_lane[5] <= 0;
        end else if (sample_ce) begin
            even_half <= even_slot[8:1]; odd_half <= odd_slot[8:1]; even_word <= ex/11'd6; odd_word <= ox/11'd6; // 先计算行半槽和列字地址，再寄存 URAM 读地址。
            addr00 <= {8'd0,even_half}*16'd222+{8'd0,even_word}; addr01 <= {8'd0,even_half}*16'd222+{8'd0,odd_word};
            addr10 <= {8'd0,odd_half}*16'd222+{8'd0,even_word}; addr11 <= {8'd0,odd_half}*16'd222+{8'd0,odd_word};
            active_pipe <= {active_pipe[4:0],sample_active}; xp_pipe <= {xp_pipe[4:0],sx[0]}; yp_pipe <= {yp_pipe[4:0],sy[0]};
            fx_pipe[0] <= sample_x[5:0]; fy_pipe[0] <= sample_y[5:0]; even_lane[0] <= (ex>>1)%11'd3; odd_lane[0] <= (ox>>1)%11'd3;
            fx_pipe[1] <= fx_pipe[0]; fy_pipe[1] <= fy_pipe[0]; even_lane[1] <= even_lane[0]; odd_lane[1] <= odd_lane[0];
            fx_pipe[2] <= fx_pipe[1]; fy_pipe[2] <= fy_pipe[1]; even_lane[2] <= even_lane[1]; odd_lane[2] <= odd_lane[1];
            fx_pipe[3] <= fx_pipe[2]; fy_pipe[3] <= fy_pipe[2]; even_lane[3] <= even_lane[2]; odd_lane[3] <= odd_lane[2];
            fx_pipe[4] <= fx_pipe[3]; fy_pipe[4] <= fy_pipe[3]; even_lane[4] <= even_lane[3]; odd_lane[4] <= odd_lane[3];
            fx_pipe[5] <= fx_pipe[4]; fy_pipe[5] <= fy_pipe[4]; even_lane[5] <= even_lane[4]; odd_lane[5] <= odd_lane[4];
        end
    end
    reg [71:0] q00_0, q00_1, q00_2, q00_3;
    always @(posedge clk) begin
        if (sample_ce) begin
            if (active_pipe[1]) q00_0 <= bank00[addr00];
            q00_1 <= q00_0; q00_2 <= q00_1; q00_3 <= q00_2;
        end
    end
    wire [23:0] rgb00;
    assign rgb00 = even_lane[5] == 0 ? q00_3[23:0] : even_lane[5] == 1 ? q00_3[47:24] : q00_3[71:48];
    reg [71:0] q01_0, q01_1, q01_2, q01_3;
    always @(posedge clk) begin
        if (sample_ce) begin
            if (active_pipe[1]) q01_0 <= bank01[addr01];
            q01_1 <= q01_0; q01_2 <= q01_1; q01_3 <= q01_2;
        end
    end
    wire [23:0] rgb01;
    assign rgb01 = odd_lane[5] == 0 ? q01_3[23:0] : odd_lane[5] == 1 ? q01_3[47:24] : q01_3[71:48];
    reg [71:0] q10_0, q10_1, q10_2, q10_3;
    always @(posedge clk) begin
        if (sample_ce) begin
            if (active_pipe[1]) q10_0 <= bank10[addr10];
            q10_1 <= q10_0; q10_2 <= q10_1; q10_3 <= q10_2;
        end
    end
    wire [23:0] rgb10;
    assign rgb10 = even_lane[5] == 0 ? q10_3[23:0] : even_lane[5] == 1 ? q10_3[47:24] : q10_3[71:48];
    reg [71:0] q11_0, q11_1, q11_2, q11_3;
    always @(posedge clk) begin
        if (sample_ce) begin
            if (active_pipe[1]) q11_0 <= bank11[addr11];
            q11_1 <= q11_0; q11_2 <= q11_1; q11_3 <= q11_2;
        end
    end
    wire [23:0] rgb11;
    assign rgb11 = odd_lane[5] == 0 ? q11_3[23:0] : odd_lane[5] == 1 ? q11_3[47:24] : q11_3[71:48];
    assign p0 = !active_pipe[5] ? 24'd0 : !yp_pipe[5] ? (!xp_pipe[5] ? rgb00 : rgb01) : (!xp_pipe[5] ? rgb10 : rgb11); // 将四个存储体的返回值按坐标奇偶重排为左上、右上、左下、右下。
    assign p1 = !active_pipe[5] ? 24'd0 : !yp_pipe[5] ? (!xp_pipe[5] ? rgb01 : rgb00) : (!xp_pipe[5] ? rgb11 : rgb10);
    assign p2 = !active_pipe[5] ? 24'd0 : !yp_pipe[5] ? (!xp_pipe[5] ? rgb10 : rgb11) : (!xp_pipe[5] ? rgb00 : rgb01);
    assign p3 = !active_pipe[5] ? 24'd0 : !yp_pipe[5] ? (!xp_pipe[5] ? rgb11 : rgb10) : (!xp_pipe[5] ? rgb01 : rgb00);
    assign fx = fx_pipe[5]; assign fy = fy_pipe[5];
endmodule
module fc_bilinear ( // 四拍 RGB888 双线性插值；X/Y 小数部分均为 Q6。
    input wire clk, input wire rst_n, input wire clear, input wire ce,
    input wire [23:0] p0, input wire [23:0] p1, input wire [23:0] p2, input wire [23:0] p3,
    input wire [5:0] fx, input wire [5:0] fy, output wire [23:0] rgb
);
    wire [6:0] wx0, wx1;
    assign wx0 = 7'd64-{1'b0,fx}; assign wx1 = {1'b0,fx};
    reg [5:0] fy0, fy1;
    always @(posedge clk) begin
        if (!rst_n) begin fy0 <= 0; fy1 <= 0; end else if (clear) begin fy0 <= 0; fy1 <= 0; end
        else if (ce) begin fy0 <= fy; fy1 <= fy0; end
    end
    reg [14:0] r0, r1, r2, r3, rtop, rbottom;
    reg [21:0] rytop, rybottom, rsum;
    always @(posedge clk) begin
        if (!rst_n) begin r0 <= 0; r1 <= 0; r2 <= 0; r3 <= 0; rtop <= 0; rbottom <= 0; rytop <= 0; rybottom <= 0; rsum <= 0; end else if (clear) begin r0 <= 0; r1 <= 0; r2 <= 0; r3 <= 0; rtop <= 0; rbottom <= 0; rytop <= 0; rybottom <= 0; rsum <= 0; end
        else if (ce) begin
            r0 <= p0[7:0]*wx0; r1 <= p1[7:0]*wx1; r2 <= p2[7:0]*wx0; r3 <= p3[7:0]*wx1;
            rtop <= r0+r1; rbottom <= r2+r3;
            rytop <= rtop*(7'd64-{1'b0,fy1}); rybottom <= rbottom*{1'b0,fy1};
            rsum <= rytop+rybottom;
        end
    end
    reg [14:0] g0, g1, g2, g3, gtop, gbottom;
    reg [21:0] gytop, gybottom, gsum;
    always @(posedge clk) begin
        if (!rst_n) begin g0 <= 0; g1 <= 0; g2 <= 0; g3 <= 0; gtop <= 0; gbottom <= 0; gytop <= 0; gybottom <= 0; gsum <= 0; end else if (clear) begin g0 <= 0; g1 <= 0; g2 <= 0; g3 <= 0; gtop <= 0; gbottom <= 0; gytop <= 0; gybottom <= 0; gsum <= 0; end
        else if (ce) begin
            g0 <= p0[15:8]*wx0; g1 <= p1[15:8]*wx1; g2 <= p2[15:8]*wx0; g3 <= p3[15:8]*wx1;
            gtop <= g0+g1; gbottom <= g2+g3;
            gytop <= gtop*(7'd64-{1'b0,fy1}); gybottom <= gbottom*{1'b0,fy1};
            gsum <= gytop+gybottom;
        end
    end
    reg [14:0] b0, b1, b2, b3, btop, bbottom;
    reg [21:0] bytop, bybottom, bsum;
    always @(posedge clk) begin
        if (!rst_n) begin b0 <= 0; b1 <= 0; b2 <= 0; b3 <= 0; btop <= 0; bbottom <= 0; bytop <= 0; bybottom <= 0; bsum <= 0; end else if (clear) begin b0 <= 0; b1 <= 0; b2 <= 0; b3 <= 0; btop <= 0; bbottom <= 0; bytop <= 0; bybottom <= 0; bsum <= 0; end
        else if (ce) begin
            b0 <= p0[23:16]*wx0; b1 <= p1[23:16]*wx1; b2 <= p2[23:16]*wx0; b3 <= p3[23:16]*wx1;
            btop <= b0+b1; bbottom <= b2+b3;
            bytop <= btop*(7'd64-{1'b0,fy1}); bybottom <= bbottom*{1'b0,fy1};
            bsum <= bytop+bybottom;
        end
    end
    assign rgb = {bsum[19:12],gsum[19:12],rsum[19:12]}; // 两次 Q6 加权后右移 12 位截断，与软件参考保持相同整数规则。
endmodule

module fc_coord_pack4 ( // 四像素坐标验证、无损打包及源行最小/最大值归约。
    input wire [127:0] x_word,y_word, input wire [63:0] weights4, input wire rear,
    output wire [135:0] packed_word, output wire bad,used, output wire [10:0] min_row,max_row
);
    wire [3:0] active,good,valid_lane,bad_lane; wire [10:0] low [0:3],high [0:3];
    genvar pi;
    generate for(pi=0;pi<4;pi=pi+1) begin: gen_pair
        wire [31:0] x,y; wire [15:0] w;
        assign x=x_word[pi*32+:32]; assign y=y_word[pi*32+:32]; assign w=weights4[pi*16+:16];
        assign active[pi]=rear ? w<16'd256 : w!=16'd0;
        assign good[pi]=!x[31] && !y[31] && x<32'd84928 && y<32'd84928; // 有贡献的采样坐标须在 [0,1327*64) 内，确保四邻点均不越界。
        assign valid_lane[pi]=active[pi] && good[pi]; assign bad_lane[pi]=active[pi] && !good[pi];
        assign packed_word[pi*34+:34]=valid_lane[pi] ? {y[16:0],x[16:0]} : 34'd0; // 每组保留 {Y[16:0],X[16:0]}，无贡献像素置零且不参与源行归约。
        assign low[pi]=valid_lane[pi] ? y[16:6] : 11'd1327;
        assign high[pi]=valid_lane[pi] ? y[16:6]+11'd1 : 11'd0;
    end endgenerate
    wire [10:0] min01,min23,max01,max23;
    assign min01=low[0]<low[1] ? low[0] : low[1]; assign min23=low[2]<low[3] ? low[2] : low[3];
    assign max01=high[0]>high[1] ? high[0] : high[1]; assign max23=high[2]>high[3] ? high[2] : high[3];
    assign min_row=min01<min23 ? min01 : min23; assign max_row=max01>max23 ? max01 : max23;
    assign bad=|bad_lane; assign used=|valid_lane;
endmodule
