`include "jpeg_global.v"
// RGBX 全景 JPEG 编码，复用开源 DCT、量化核心及 Huffman 表。
module mjpeg_encoder_core #(parameter integer WIDTH=2656,parameter integer HEIGHT=1328)(
    (* X_INTERFACE_PARAMETER="ASSOCIATED_BUSIF S_AXIS:M_AXIS, ASSOCIATED_RESET aresetn" *)
    input wire aclk,input wire aresetn,
    input wire [31:0] s_axis_tdata,input wire [3:0] s_axis_tkeep,input wire s_axis_tlast,s_axis_tvalid,output wire s_axis_tready,
    output wire [31:0] m_axis_tdata,output wire [3:0] m_axis_tkeep,output wire m_axis_tlast,m_axis_tvalid,input wire m_axis_tready,
    input wire [31:0] run,output wire [31:0] status
);
    localparam [11:0] DEFAULT_WIDTH=WIDTH[11:0];localparam [10:0] DEFAULT_HEIGHT=HEIGHT[10:0];
    wire run_enable;wire [11:0] requested_width,selected_width;wire [10:0] selected_height; // run[0] 使能；宽度为 {run[24],run[11:1]}，高度为 run[22:12]，零值选默认尺寸。
    assign run_enable=aresetn && run[0];assign requested_width={run[24],run[11:1]};
    assign selected_width=requested_width==12'd0 ? DEFAULT_WIDTH : requested_width;
    assign selected_height=run[22:12]==11'd0 ? DEFAULT_HEIGHT : run[22:12];
    reg configured;reg [5:0] init_count;reg [11:0] frame_width;reg [10:0] frame_height;reg [31:0] frame_pixels,pixels;
    reg [23:0] rgb,yuv;reg rgb_valid,cam_valid,input_error,sensor_error_sticky,entropy_error_sticky;
    wire encoder_rstn,frame_start,cam_ready,sensor_error,entropy_error,overflow,frame_done;
    assign encoder_rstn=run_enable && configured;assign frame_start=encoder_rstn && init_count==6'd0 && !input_error;
    assign s_axis_tready=encoder_rstn && init_count==6'd63 && !input_error && !sensor_error_sticky && !entropy_error_sticky && !overflow && // 两级颜色转换随下游就绪推进；帧初始化完成后才接收像素。
                         pixels<frame_pixels && (!cam_valid || cam_ready);
    assign status={27'd0,input_error,entropy_error_sticky,sensor_error_sticky,overflow,frame_done}; // status[4:0] 依次为输入错误、熵编码错误、条带错误、溢出、完成。
    wire signed [18:0] y_sum,u_sum,v_sum;
    assign y_sum=19'sd77*$signed({1'b0,rgb[7:0]})+19'sd150*$signed({1'b0,rgb[15:8]})+19'sd29*$signed({1'b0,rgb[23:16]})+19'sd128; // 输入低字节为 R/G/B/X；定点 BT.601 转换，四舍五入后限幅至 0..255。
    assign u_sum=-19'sd43*$signed({1'b0,rgb[7:0]})-19'sd85*$signed({1'b0,rgb[15:8]})+19'sd128*$signed({1'b0,rgb[23:16]})+19'sd32896;
    assign v_sum=19'sd128*$signed({1'b0,rgb[7:0]})-19'sd107*$signed({1'b0,rgb[15:8]})-19'sd21*$signed({1'b0,rgb[23:16]})+19'sd32896;
    always @(posedge aclk) begin
        if(!aresetn) begin
            configured<=1'b0;init_count<=6'd0;frame_width<=DEFAULT_WIDTH;frame_height<=DEFAULT_HEIGHT;frame_pixels<=32'd0;pixels<=32'd0;
            rgb<=24'd0;yuv<=24'd0;rgb_valid<=1'b0;cam_valid<=1'b0;input_error<=1'b0;sensor_error_sticky<=1'b0;entropy_error_sticky<=1'b0;
        end else if(!run[0]) begin
            configured<=1'b0;init_count<=6'd0;frame_width<=DEFAULT_WIDTH;frame_height<=DEFAULT_HEIGHT;frame_pixels<=32'd0;pixels<=32'd0;
            rgb<=24'd0;yuv<=24'd0;rgb_valid<=1'b0;cam_valid<=1'b0;input_error<=1'b0;sensor_error_sticky<=1'b0;entropy_error_sticky<=1'b0;
        end else if(!configured) begin
            configured<=1'b1;frame_width<=selected_width;frame_height<=selected_height;frame_pixels<={20'd0,selected_width}*{21'd0,selected_height};
            input_error<=selected_width==12'd0 || selected_width>12'd2656 || selected_width[2:0]!=3'd0 || selected_height==11'd0 || selected_height>11'd1328 || selected_height[2:0]!=3'd0; // 尺寸须为 8 的倍数，且不超过 2656x1328。
        end else begin
            if(init_count!=6'd63) init_count<=init_count+6'd1;
            if(sensor_error) sensor_error_sticky<=1'b1;if(entropy_error) entropy_error_sticky<=1'b1;
            if(!cam_valid || cam_ready) begin
                cam_valid<=rgb_valid;rgb_valid<=s_axis_tvalid && s_axis_tready;
                if(rgb_valid) begin
                    yuv[7:0]<=y_sum<19'sd0 ? 8'd0 : y_sum>=19'sd65536 ? 8'd255 : y_sum[15:8];
                    yuv[15:8]<=u_sum<19'sd0 ? 8'd0 : u_sum>=19'sd65536 ? 8'd255 : u_sum[15:8];
                    yuv[23:16]<=v_sum<19'sd0 ? 8'd0 : v_sum>=19'sd65536 ? 8'd255 : v_sum[15:8];
                end
                if(s_axis_tvalid && s_axis_tready) begin
                    rgb<=s_axis_tdata[23:0];pixels<=pixels+32'd1;
                    if(s_axis_tkeep!=4'hf || s_axis_tlast!=(pixels==frame_pixels-32'd1)) input_error<=1'b1; // 每像素四个字节均有效，TLAST 只允许出现在本帧最后一个像素。
                end
            end
        end
    end
    panorama_jpeg_encoder encoder(
        .clk(aclk),.rstn(encoder_rstn),.start(frame_start),.width(frame_width),.height(frame_height),
        .pixel_valid(cam_valid),.pixel_ready(cam_ready),.pixel_yuv(yuv),
        .out_data(m_axis_tdata),.out_keep(m_axis_tkeep),.out_last(m_axis_tlast),.out_valid(m_axis_tvalid),.out_ready(m_axis_tready),
        .done(frame_done),.overflow(overflow),.sensor_error(sensor_error),.entropy_error(entropy_error)
    );
endmodule

// 奇偶 BRAM 同时预读相邻码字；同址读写显式转发，支持每拍弹出两个。
module panorama_token_fifo(
    input wire clk,rstn,input wire push,input wire [34:0] in_token,input wire [1:0] pop_count,
    output wire [34:0] head0,head1,output wire [9:0] count,output reg error
);
    (* ram_style="block" *) reg [34:0] even_mem[0:255],odd_mem[0:255];
    reg [8:0] wr_ptr,rd_ptr;reg [9:0] used;reg [34:0] even_q,odd_q,forward_q;reg even_forward,odd_forward;
    wire [1:0] accepted_pop;wire [8:0] next_read;wire [7:0] even_address,odd_address;wire take_push;
    assign accepted_pop=pop_count<=2'd2 && {8'd0,pop_count}<=used ? pop_count : 2'd0;
    assign next_read=rd_ptr+{7'd0,accepted_pop};assign odd_address=next_read[8:1];assign even_address=next_read[8:1]+{7'd0,next_read[0]};
    assign take_push=push && (used<10'd512 || accepted_pop!=2'd0);
    assign head0=rd_ptr[0] ? (odd_forward ? forward_q : odd_q) : (even_forward ? forward_q : even_q);
    assign head1=rd_ptr[0] ? (even_forward ? forward_q : even_q) : (odd_forward ? forward_q : odd_q);assign count=used;
    always @(posedge clk) begin
        if(rstn && take_push && !wr_ptr[0]) even_mem[wr_ptr[8:1]]<=in_token;
        if(rstn && take_push && wr_ptr[0]) odd_mem[wr_ptr[8:1]]<=in_token;
        even_q<=even_mem[even_address];odd_q<=odd_mem[odd_address];forward_q<=in_token;
        even_forward<=rstn && take_push && !wr_ptr[0] && wr_ptr[8:1]==even_address;
        odd_forward<=rstn && take_push && wr_ptr[0] && wr_ptr[8:1]==odd_address;
    end
    always @(posedge clk) begin
        if(!rstn) begin wr_ptr<=9'd0;rd_ptr<=9'd0;used<=10'd0;error<=1'b0;end
        else begin
            if(push && !take_push || pop_count==2'd3 || {8'd0,pop_count}>used) error<=1'b1;
            if(take_push) wr_ptr<=wr_ptr+9'd1;
            rd_ptr<=next_read;used<=used+{9'd0,take_push}-{8'd0,accepted_pop};
        end
    end
endmodule

// 三分量独立执行 DC 差分、AC 游程和 Huffman 编码，每路每拍至多输出一个码字。
module panorama_entropy_lane #(parameter [1:0] COMPONENT=2'd0)(
    input wire clk,rstn,start,input wire [3:0] slot,input wire frame_last,
    output wire [8:0] coeff_address,input wire [95:0] coeff_data,
    output reg token_valid,output reg [34:0] token,output wire done,output reg error
);
    localparam [1:0] S_IDLE=2'd0,S_LOAD=2'd1,S_RLE=2'd2,S_WAIT=2'd3;
    reg [1:0] state;reg [3:0] slot_q;reg last_q,load_running,load_valid;reg [2:0] load_index,load_index_q;
    reg [95:0] coeff_cache[0:7];reg [5:0] zig_index,zero_run;reg signed [11:0] previous_dc;
    reg [5:0] natural;wire signed [11:0] coefficient;wire signed [12:0] difference;wire [12:0] magnitude;
    assign coefficient=coeff_cache[natural[5:3]][12*(7-natural[2:0])+:12];
    assign difference=zig_index==6'd0 ? $signed({coefficient[11],coefficient})-$signed({previous_dc[11],previous_dc}) : $signed({coefficient[11],coefficient});
    assign magnitude=difference<13'sd0 ? -difference : difference;
    assign coeff_address={slot_q,5'd0}+{2'd0,COMPONENT,3'd0}+{6'd0,load_index};
    reg [3:0] category;always @* begin
        category=4'd0;
        if(magnitude[0]) category=4'd1;if(magnitude[1]) category=4'd2;if(magnitude[2]) category=4'd3;if(magnitude[3]) category=4'd4;
        if(magnitude[4]) category=4'd5;if(magnitude[5]) category=4'd6;if(magnitude[6]) category=4'd7;if(magnitude[7]) category=4'd8;
        if(magnitude[8]) category=4'd9;if(magnitude[9]) category=4'd10;if(magnitude[10]) category=4'd11;if(magnitude[11]) category=4'd12;if(magnitude[12]) category=4'd13;
    end
    // Zigzag 顺序沿用原 entropy_encoder 的转置坐标约定。
    always @* begin
        case(zig_index)
            6'd0:natural=6'd0;
            6'd1:natural=6'd8;
            6'd2:natural=6'd1;
            6'd3:natural=6'd2;
            6'd4:natural=6'd9;
            6'd5:natural=6'd16;
            6'd6:natural=6'd24;
            6'd7:natural=6'd17;
            6'd8:natural=6'd10;
            6'd9:natural=6'd3;
            6'd10:natural=6'd4;
            6'd11:natural=6'd11;
            6'd12:natural=6'd18;
            6'd13:natural=6'd25;
            6'd14:natural=6'd32;
            6'd15:natural=6'd40;
            6'd16:natural=6'd33;
            6'd17:natural=6'd26;
            6'd18:natural=6'd19;
            6'd19:natural=6'd12;
            6'd20:natural=6'd5;
            6'd21:natural=6'd6;
            6'd22:natural=6'd13;
            6'd23:natural=6'd20;
            6'd24:natural=6'd27;
            6'd25:natural=6'd34;
            6'd26:natural=6'd41;
            6'd27:natural=6'd48;
            6'd28:natural=6'd56;
            6'd29:natural=6'd49;
            6'd30:natural=6'd42;
            6'd31:natural=6'd35;
            6'd32:natural=6'd28;
            6'd33:natural=6'd21;
            6'd34:natural=6'd14;
            6'd35:natural=6'd7;
            6'd36:natural=6'd15;
            6'd37:natural=6'd22;
            6'd38:natural=6'd29;
            6'd39:natural=6'd36;
            6'd40:natural=6'd43;
            6'd41:natural=6'd50;
            6'd42:natural=6'd57;
            6'd43:natural=6'd58;
            6'd44:natural=6'd51;
            6'd45:natural=6'd44;
            6'd46:natural=6'd37;
            6'd47:natural=6'd30;
            6'd48:natural=6'd23;
            6'd49:natural=6'd31;
            6'd50:natural=6'd38;
            6'd51:natural=6'd45;
            6'd52:natural=6'd52;
            6'd53:natural=6'd59;
            6'd54:natural=6'd60;
            6'd55:natural=6'd53;
            6'd56:natural=6'd46;
            6'd57:natural=6'd39;
            6'd58:natural=6'd47;
            6'd59:natural=6'd54;
            6'd60:natural=6'd61;
            6'd61:natural=6'd62;
            6'd62:natural=6'd55;
            6'd63:natural=6'd63;
            default:natural=6'd0;
        endcase
    end
    reg symbol_valid,symbol_dc,symbol_last,symbol_frame;reg [3:0] symbol_size,symbol_run;reg [11:0] symbol_amplitude;
    reg valid_d,dc_d,last_d,frame_d;reg [3:0] size_d;reg [11:0] amplitude_d;
    wire [3:0] dc_length;wire [10:0] dc_code;wire [4:0] ac_length;wire [15:0] ac_code;
    generate if(COMPONENT==2'd0) begin:gen_luma
        wire [8:0] luma_dc_code;
        dc_luma_rom dc_rom(.clk(clk),.VLI_size(symbol_size),.VLC_DC_size(dc_length),.VLC_DC(luma_dc_code));
        assign dc_code={2'd0,luma_dc_code};
        ac_luma_rom ac_rom(.clk(clk),.runlength(symbol_run),.VLI_size(symbol_size),.VLC_AC_size(ac_length),.VLC_AC(ac_code));
    end else begin:gen_chroma
        dc_chroma_rom dc_rom(.clk(clk),.VLI_size(symbol_size),.VLC_DC_size(dc_length),.VLC_DC(dc_code));
        ac_chroma_rom ac_rom(.clk(clk),.runlength(symbol_run),.VLI_size(symbol_size),.VLC_AC_size(ac_length),.VLC_AC(ac_code));
    end endgenerate
    wire [4:0] code_length;wire [27:0] code_value,amplitude_mask,combined_code;wire [4:0] combined_length;
    assign code_length=dc_d ? {1'b0,dc_length} : ac_length;assign code_value=dc_d ? {17'd0,dc_code} : {12'd0,ac_code};
    assign amplitude_mask=(28'd1<<size_d)-28'd1;assign combined_code=(code_value<<size_d)|({16'd0,amplitude_d}&amplitude_mask);
    assign combined_length=code_length+{1'b0,size_d};assign done=token_valid && token[33];
    always @(posedge clk) begin
        if(load_valid) coeff_cache[load_index_q]<=coeff_data;
    end
    always @(posedge clk) begin
        if(!rstn) begin
            state<=S_IDLE;slot_q<=4'd0;last_q<=1'b0;load_running<=1'b0;load_valid<=1'b0;load_index<=3'd0;load_index_q<=3'd0;
            zig_index<=6'd0;zero_run<=6'd0;previous_dc<=12'sd0;error<=1'b0;
            symbol_valid<=1'b0;symbol_dc<=1'b0;symbol_last<=1'b0;symbol_frame<=1'b0;symbol_size<=4'd0;symbol_run<=4'd0;symbol_amplitude<=12'd0;
            valid_d<=1'b0;dc_d<=1'b0;last_d<=1'b0;frame_d<=1'b0;size_d<=4'd0;amplitude_d<=12'd0;token_valid<=1'b0;token<=35'd0;
        end else begin
            symbol_valid<=1'b0;symbol_last<=1'b0;symbol_frame<=1'b0;
            load_valid<=state==S_LOAD && load_running;load_index_q<=load_index;
            valid_d<=symbol_valid;dc_d<=symbol_dc;last_d<=symbol_last;frame_d<=symbol_frame;size_d<=symbol_size;amplitude_d<=symbol_amplitude; // 幅值及结束标志随同步 Huffman ROM 延迟一拍，再拼接输出码字。
            token_valid<=valid_d;token<={frame_d,last_d,combined_length,combined_code}; // 码字 35 位：{帧尾,块尾,5 位有效长度,28 位右对齐码字}。
            if(valid_d && (code_length==5'd0 || combined_length>5'd28)) error<=1'b1;
            case(state)
                S_IDLE:if(start) begin state<=S_LOAD;slot_q<=slot;last_q<=frame_last;load_running<=1'b1;load_index<=3'd0;zig_index<=6'd0;zero_run<=6'd0;end
                S_LOAD:begin
                    if(load_running) begin if(load_index==3'd7) load_running<=1'b0;else load_index<=load_index+3'd1;end
                    if(load_valid && load_index_q==3'd7) state<=S_RLE;
                end
                S_RLE:begin
                    if(zig_index==6'd0) begin
                        symbol_valid<=1'b1;symbol_dc<=1'b1;symbol_size<=category;symbol_run<=4'd0;symbol_amplitude<=difference<13'sd0 ? difference-13'sd1 : difference;
                        previous_dc<=last_q ? 12'sd0 : coefficient;zig_index<=6'd1;
                        if(category>4'd11) error<=1'b1;
                    end else if(coefficient==12'sd0) begin
                        if(zig_index==6'd63) begin
                            symbol_valid<=1'b1;symbol_dc<=1'b0;symbol_size<=4'd0;symbol_run<=4'd0;symbol_amplitude<=12'd0;symbol_last<=1'b1;symbol_frame<=last_q && COMPONENT==2'd2;state<=S_WAIT;
                        end else begin zero_run<=zero_run+6'd1;zig_index<=zig_index+6'd1;end
                    end else if(zero_run>=6'd16) begin
                        symbol_valid<=1'b1;symbol_dc<=1'b0;symbol_size<=4'd0;symbol_run<=4'd15;symbol_amplitude<=12'd0;zero_run<=zero_run-6'd16;
                    end else begin
                        symbol_valid<=1'b1;symbol_dc<=1'b0;symbol_size<=category;symbol_run<=zero_run[3:0];symbol_amplitude<=difference<13'sd0 ? difference-13'sd1 : difference;
                        zero_run<=6'd0;
                        if(category>4'd10) error<=1'b1;
                        if(zig_index==6'd63) begin symbol_last<=1'b1;symbol_frame<=last_q && COMPONENT==2'd2;state<=S_WAIT;end else zig_index<=zig_index+6'd1;
                    end
                end
                S_WAIT:if(done) state<=S_IDLE;
                default:begin state<=S_IDLE;error<=1'b1;end
            endcase
        end
    end
endmodule
// 两个八行条带：各分量 5312 个 64 位字，仅完整条带可发布给 DCT。
module panorama_stripe_cache(
    input wire clk,rstn,start,input wire [11:0] width,input wire [10:0] height,
    input wire pixel_valid,output wire pixel_ready,input wire [23:0] pixel_yuv,
    input wire take_slot,release_slot,input wire read_slot,output wire [1:0] ready_slots,
    input wire read_n,input wire [`W_ACAMBUF:0] read_address,output wire [63:0] read_data,output reg error
);
    localparam [1:0] EMPTY=2'd0,FILLING=2'd1,READY=2'd2,READING=2'd3;
    (* ram_style="block" *) reg [63:0] y_mem[0:5311],u_mem[0:5311],v_mem[0:5311];
    reg [1:0] slot_state[0:1];reg fill_slot;reg [11:0] x;reg [10:0] y;reg [63:0] pack_y,pack_u,pack_v;
    wire pixel_take;wire [12:0] write_address,local_read;wire [1:0] component_read;
    reg [63:0] y_q,u_q,v_q;reg [1:0] component_q;
    assign pixel_ready=rstn && !start && y<height && (slot_state[fill_slot]==EMPTY || slot_state[fill_slot]==FILLING);
    assign pixel_take=pixel_valid && pixel_ready;
    assign write_address=(fill_slot ? 13'd2656 : 13'd0)+{10'd0,y[2:0]}*13'd332+{4'd0,x[11:3]};
    assign component_read=read_address>=15'd10624 ? 2'd2 : read_address>=15'd5312 ? 2'd1 : 2'd0;
    assign local_read=read_address-(component_read==2'd2 ? 15'd10624 : component_read==2'd1 ? 15'd5312 : 15'd0);
    assign read_data=component_q==2'd0 ? y_q : component_q==2'd1 ? u_q : v_q;
    assign ready_slots={slot_state[1]==READY,slot_state[0]==READY};
    always @(posedge clk) begin
        if(rstn && !start && pixel_take && x[2:0]==3'd7) begin
            y_mem[write_address]<={pack_y[55:0],pixel_yuv[7:0]};u_mem[write_address]<={pack_u[55:0],pixel_yuv[15:8]};v_mem[write_address]<={pack_v[55:0],pixel_yuv[23:16]};
        end
        if(!read_n && local_read<13'd5312) begin y_q<=y_mem[local_read];u_q<=u_mem[local_read];v_q<=v_mem[local_read];component_q<=component_read;end
    end
    always @(posedge clk) begin
        if(!rstn) begin
            x<=12'd0;y<=11'd0;fill_slot<=1'b0;pack_y<=64'd0;pack_u<=64'd0;pack_v<=64'd0;slot_state[0]<=EMPTY;slot_state[1]<=EMPTY;error<=1'b0;
        end else if(start) begin
            x<=12'd0;y<=11'd0;fill_slot<=1'b0;pack_y<=64'd0;pack_u<=64'd0;pack_v<=64'd0;slot_state[0]<=EMPTY;slot_state[1]<=EMPTY;error<=1'b0;
        end else begin
            if(take_slot) begin if(slot_state[read_slot]!=READY) error<=1'b1;else slot_state[read_slot]<=READING;end // 槽状态 EMPTY→FILLING→READY→READING→EMPTY；完整读取后才能复用。
            if(release_slot) begin if(slot_state[read_slot]!=READING) error<=1'b1;else slot_state[read_slot]<=EMPTY;end
            if(!read_n && read_address>=15'd15936) error<=1'b1;
            if(pixel_take) begin
                pack_y<={pack_y[55:0],pixel_yuv[7:0]};pack_u<={pack_u[55:0],pixel_yuv[15:8]};pack_v<={pack_v[55:0],pixel_yuv[23:16]};
                slot_state[fill_slot]<=FILLING;
                if(x==width-12'd1) begin
                    x<=12'd0;y<=y+11'd1;
                    if(y[2:0]==3'd7) begin slot_state[fill_slot]<=READY;fill_slot<=!fill_slot;end
                end else x<=x+12'd1;
            end
        end
    end
endmodule

// 共享 fdct_quant，预留四 MCU 信用；三路独立熵编码后按 Y/Cb/Cr 顺序合并。
module panorama_jpeg_encoder(
    input wire clk,rstn,start,input wire [11:0] width,input wire [10:0] height,
    input wire pixel_valid,output wire pixel_ready,input wire [23:0] pixel_yuv,
    output wire [31:0] out_data,output wire [3:0] out_keep,output wire out_last,out_valid,input wire out_ready,
    output wire done,overflow,sensor_error,entropy_error
);
    wire logic_rstn;assign logic_rstn=rstn && !start;
    wire [8:0] columns;wire [7:0] strips;wire [16:0] total_mcus;
    assign columns=width[11:3];assign strips=height[10:3];assign total_mcus={8'd0,columns}*{9'd0,strips};
    reg [8:0] mb_col;reg [7:0] mb_row;reg dct_busy,dct_finished,lane_busy;reg [2:0] lane_finished;
    reg [2:0] reserved,coeff_ready;reg [3:0] coeff_head;reg [16:0] entropy_mcu;
    wire [1:0] slots_ready;wire stripe_error,fdct_go,fdct_ready,quant_ready,ee_start,ee_release,last_col,last_row;
    wire [`W_ACAMBUF:0] camera_address;wire camera_read_n;wire [63:0] camera_data;
    wire quant_write_n;wire [4:0] quant_address;wire [95:0] quant_data;wire [3:0] quant_slot;
    assign last_col=mb_col==columns-9'd1;assign last_row=mb_row==strips-8'd1;
    assign fdct_go=logic_rstn && !dct_finished && !dct_busy && reserved<3'd4 && (mb_col!=9'd0 || slots_ready[mb_row[0]]); // 启动 DCT 前预留系数槽，条带首列还须确认像素条带已就绪。
    panorama_stripe_cache stripes(
        .clk(clk),.rstn(rstn),.start(start),.width(width),.height(height),.pixel_valid(pixel_valid),.pixel_ready(pixel_ready),.pixel_yuv(pixel_yuv),
        .take_slot(fdct_go && mb_col==9'd0),.release_slot(fdct_ready && last_col),.read_slot(mb_row[0]),.ready_slots(slots_ready),
        .read_n(camera_read_n),.read_address(camera_address),.read_data(camera_data),.error(stripe_error)
    );
    fdct_quant quantizer(
        .clk(clk),.rstn(logic_rstn),.fdct_go(fdct_go),.last_mb_in_pic_i(last_col && last_row),.last_mb_in_row_i(last_col),.MB_Col(mb_col),.MB_Row(mb_row),
        .aa_camera_buf(camera_address),.cena_camera_buf(camera_read_n),.qa_camera_buf(camera_data),.ready_for_next(fdct_ready),
        .cenb_ee_buf(quant_write_n),.ab_ee_buf(quant_address),.db_ee_buf(quant_data),.wid_ee_buf(quant_slot),.wr_ee_buf_ready(quant_ready)
    );
    wire [34:0] h0[0:2],h1[0:2],lane_token[0:2];wire [9:0] token_count[0:2];wire [1:0] pop_count[0:2];
    wire [2:0] lane_done,lane_error,fifo_error,token_push;
    wire [8:0] coeff_address[0:2];wire [95:0] coeff_q[0:2];
    assign ee_start=logic_rstn && !lane_busy && coeff_ready!=3'd0 && token_count[0]<=10'd446 && token_count[1]<=10'd446 && token_count[2]<=10'd446; // 启动熵编码前为每个分量最坏 66 个码字预留 FIFO 容量。
    assign ee_release=lane_busy && (lane_finished|lane_done)==3'b111; // 三个分量均结束后才释放对应 MCU 信用。
    genvar component;
    generate for(component=0;component<3;component=component+1) begin:gen_entropy
        (* ram_style="block" *) reg [95:0] coeff_mem[0:511];reg [95:0] coeff_read;
        always @(posedge clk) begin
            if(logic_rstn && !quant_write_n) coeff_mem[{quant_slot,quant_address}]<=quant_data;
            coeff_read<=coeff_mem[coeff_address[component]];
        end
        assign coeff_q[component]=coeff_read;
        panorama_entropy_lane #(.COMPONENT(component)) lane(
            .clk(clk),.rstn(logic_rstn),.start(ee_start),.slot(coeff_head),.frame_last(entropy_mcu==total_mcus-17'd1),
            .coeff_address(coeff_address[component]),.coeff_data(coeff_q[component]),.token_valid(token_push[component]),.token(lane_token[component]),.done(lane_done[component]),.error(lane_error[component])
        );
        panorama_token_fifo tokens(
            .clk(clk),.rstn(logic_rstn),.push(token_push[component]),.in_token(lane_token[component]),.pop_count(pop_count[component]),
            .head0(h0[component]),.head1(h1[component]),.count(token_count[component]),.error(fifo_error[component])
        );
    end endgenerate
    reg schedule_error;
    always @(posedge clk) begin
        if(!rstn) begin
            mb_col<=9'd0;mb_row<=8'd0;dct_busy<=1'b0;dct_finished<=1'b0;lane_busy<=1'b0;lane_finished<=3'd0;
            reserved<=3'd0;coeff_ready<=3'd0;coeff_head<=4'd0;entropy_mcu<=17'd0;schedule_error<=1'b0;
        end else if(start) begin
            mb_col<=9'd0;mb_row<=8'd0;dct_busy<=1'b0;dct_finished<=1'b0;lane_busy<=1'b0;lane_finished<=3'd0;
            reserved<=3'd0;coeff_ready<=3'd0;coeff_head<=4'd0;entropy_mcu<=17'd0;schedule_error<=1'b0;
        end else begin
            if(fdct_go) dct_busy<=1'b1;
            if(fdct_ready) begin
                dct_busy<=1'b0;
                if(last_col) begin mb_col<=9'd0;if(last_row) dct_finished<=1'b1;else mb_row<=mb_row+8'd1;end
                else mb_col<=mb_col+9'd1;
            end
            if(ee_start) begin lane_busy<=1'b1;lane_finished<=3'd0;coeff_head<=coeff_head+4'd1;entropy_mcu<=entropy_mcu+17'd1;end
            else if(lane_busy) begin lane_finished<=lane_finished|lane_done;if(ee_release) lane_busy<=1'b0;end
            case({fdct_go,ee_release})
                2'b10:reserved<=reserved+3'd1;2'b01:reserved<=reserved-3'd1;default:reserved<=reserved;
            endcase
            case({quant_ready,ee_start})
                2'b10:coeff_ready<=coeff_ready+3'd1;2'b01:coeff_ready<=coeff_ready-3'd1;default:coeff_ready<=coeff_ready;
            endcase
            if(reserved>3'd4 || coeff_ready>3'd4 || quant_ready && coeff_ready==3'd4 && !ee_start) schedule_error<=1'b1;
        end
    end
    wire [34:0] first_token,second_token;wire [1:0] offered;wire code_ready;
    panorama_entropy_merge merger(
        .clk(clk),.rstn(logic_rstn),.h0_y(h0[0]),.h1_y(h1[0]),.h0_u(h0[1]),.h1_u(h1[1]),.h0_v(h0[2]),.h1_v(h1[2]),
        .count_y(token_count[0]),.count_u(token_count[1]),.count_v(token_count[2]),.pop_y(pop_count[0]),.pop_u(pop_count[1]),.pop_v(pop_count[2]),
        .offered(offered),.first_token(first_token),.second_token(second_token),.code_ready(code_ready)
    );
    wire header_valid;wire [7:0] header_byte;wire packing_error;
    header_gen header(.clk(clk),.rstn(rstn),.header_go(start),.eoi(1'b0),.PicWidth_i(width),.PicHeight_i(height),.header_byte_f(header_valid),.header_byte(header_byte),.header_ready(),.frame_ready());
    panorama_jpeg_stream stream(
        .clk(clk),.rstn(rstn),.start(start),.header_valid(header_valid),.header_byte(header_byte),
        .code_count(offered),.code0(first_token[27:0]),.length0(first_token[32:28]),.last0(first_token[34]),
        .code1(second_token[27:0]),.length1(second_token[32:28]),.last1(second_token[34]),.code_ready(code_ready),
        .out_data(out_data),.out_keep(out_keep),.out_last(out_last),.out_valid(out_valid),.out_ready(out_ready),.done(done),.overflow(overflow),.error(packing_error)
    );
    assign sensor_error=stripe_error;assign entropy_error=|lane_error || |fifo_error || schedule_error || packing_error;
endmodule
// MSB 优先拼接位流；每拍最多接收两码字，FF 填零后最多输出四字节。
module panorama_jpeg_stream(
    input wire clk,rstn,start,input wire header_valid,input wire [7:0] header_byte,
    input wire [1:0] code_count,input wire [27:0] code0,code1,input wire [4:0] length0,length1,input wire last0,last1,output wire code_ready,
    output reg [31:0] out_data,output reg [3:0] out_keep,output reg out_last,out_valid,input wire out_ready,
    output reg done,overflow,error
);
    localparam [2:0] S_HEADER=3'd0,S_ENTROPY=3'd1,S_EOI_FF=3'd2,S_EOI_D9=3'd3,S_DONE=3'd4;
    reg [2:0] state;reg [9:0] header_count;reg [127:0] bits;reg [7:0] bit_count;reg last_seen;
    (* ram_style="block" *) reg [36:0] output_fifo[0:4095];
    reg [11:0] wr_ptr,rd_ptr;reg [12:0] used;reg [31:0] pack_data;reg [1:0] pack_count;
    wire pop,space,code_take;wire [6:0] length_sum;
    assign pop=used!=13'd0 && (!out_valid || out_ready);assign space=used<13'd4096 || pop;
    assign code_ready=state==S_ENTROPY && bit_count<=8'd64 && !last_seen && !error && code_count<=2'd2;
    assign code_take=code_ready && code_count!=2'd0;assign length_sum={2'd0,length0}+(code_count==2'd2 ? {2'd0,length1} : 7'd0);
    reg [31:0] byte_data;reg [2:0] byte_count,raw_count;reg scanning;reg [7:0] candidate;integer i;
    always @* begin
        byte_data=32'd0;byte_count=3'd0;raw_count=3'd0;scanning=1'b1;candidate=8'd0;
        if(space) begin
            if(state==S_HEADER && header_valid) begin byte_data={24'd0,header_byte};byte_count=3'd1;end
            else if(state==S_EOI_FF) begin byte_data=32'h000000ff;byte_count=3'd1;end
            else if(state==S_EOI_D9) begin byte_data=32'h000000d9;byte_count=3'd1;end
            else if(state==S_ENTROPY) begin
                for(i=0;i<4;i=i+1) begin
                    candidate=bits[127-i*8-:8];
                    if(scanning && bit_count>=(i+1)*8) begin
                        if({1'b0,byte_count}+(candidate==8'hff ? 4'd2 : 4'd1)<4'd5) begin
                            byte_data[byte_count*8+:8]=candidate;byte_count=byte_count+3'd1;
                            if(candidate==8'hff) begin byte_data[byte_count*8+:8]=8'd0;byte_count=byte_count+3'd1;end
                            raw_count=raw_count+3'd1;
                        end else scanning=1'b0;
                    end else scanning=1'b0;
                end
            end
        end
    end
    reg [127:0] next_bits;wire [7:0] next_bit_count;
    always @* begin
        next_bits=bits;
        if(code_take) begin
            next_bits=next_bits|({100'd0,code0}<<(128-bit_count-length0));
            if(code_count==2'd2) next_bits=next_bits|({100'd0,code1}<<(128-bit_count-length0-length1));
        end
        next_bits=next_bits<<({5'd0,raw_count}*8);
    end
    assign next_bit_count=bit_count+(code_take ? {1'b0,length_sum} : 8'd0)-{2'd0,raw_count,3'd0};
    wire [55:0] pack_combined;wire [3:0] pack_total;wire final_byte,push;
    assign pack_combined={24'd0,pack_data}|({24'd0,byte_data}<<({4'd0,pack_count}*8));
    assign pack_total={2'd0,pack_count}+{1'b0,byte_count};assign final_byte=state==S_EOI_D9 && byte_count==3'd1;
    assign push=byte_count!=3'd0 && (pack_total>=4'd4 || final_byte);
    wire [3:0] pushed_keep;assign pushed_keep=pack_total>=4'd4 ? 4'hf : (4'hf>>(4'd4-pack_total)); // 末字按实际字节数生成 TKEEP，EOI 最后字节所在字携带 TLAST。
    always @(posedge clk) begin
        if(rstn && !start && push) output_fifo[wr_ptr]<={final_byte,pushed_keep,pack_combined[31:0]};
    end
    always @(posedge clk) begin
        if(!rstn) begin
            state<=S_HEADER;header_count<=10'd0;bits<=128'd0;bit_count<=8'd0;last_seen<=1'b0;
            wr_ptr<=12'd0;rd_ptr<=12'd0;used<=13'd0;pack_data<=32'd0;pack_count<=2'd0;done<=1'b0;overflow<=1'b0;error<=1'b0;
            out_data<=32'd0;out_keep<=4'd0;out_last<=1'b0;out_valid<=1'b0;
        end else if(start) begin
            state<=S_HEADER;header_count<=10'd0;bits<=128'd0;bit_count<=8'd0;last_seen<=1'b0;
            wr_ptr<=12'd0;rd_ptr<=12'd0;used<=13'd0;pack_data<=32'd0;pack_count<=2'd0;done<=1'b0;overflow<=1'b0;error<=1'b0;
            out_data<=32'd0;out_keep<=4'd0;out_last<=1'b0;out_valid<=1'b0;
        end else begin
            bits<=next_bits;bit_count<=next_bit_count;
            if(code_take) begin
                if(length0==5'd0 || length0>5'd28 || (code0>>length0)!=28'd0 ||
                   code_count==2'd2 && (last0 || length1==5'd0 || length1>5'd28 || (code1>>length1)!=28'd0)) error<=1'b1;
                if(last0 || code_count==2'd2 && last1) last_seen<=1'b1;
            end
            if(bit_count>8'd128 || code_count==2'd3 && state==S_ENTROPY) error<=1'b1;
            if(state==S_HEADER && header_valid) begin
                if(!space) begin overflow<=1'b1;error<=1'b1;end
                else if(header_count==10'd604) begin state<=S_ENTROPY;header_count<=10'd605;end
                else header_count<=header_count+10'd1;
            end
            if(state==S_ENTROPY && last_seen) begin // 熵码尾部不足一字节时填 1，对齐后追加 JPEG EOI 标记。
                if(bit_count==8'd0) state<=S_EOI_FF;
                else if(bit_count<8'd8) begin
                    bits<=bits|((128'hff000000000000000000000000000000>>bit_count)&128'hff000000000000000000000000000000);bit_count<=8'd8;
                end
            end
            if(state==S_EOI_FF && space) state<=S_EOI_D9;
            if(final_byte) state<=S_DONE;
            if(byte_count!=3'd0) begin
                if(push) begin pack_data<={8'd0,pack_combined[55:32]};pack_count<=pack_count+byte_count[1:0];wr_ptr<=wr_ptr+12'd1;end
                else begin pack_data<=pack_combined[31:0];pack_count<=pack_count+byte_count[1:0];end
                if(final_byte) begin pack_count<=2'd0;pack_data<=32'd0;end
            end
            case({push,pop})
                2'b10:used<=used+13'd1;2'b01:used<=used-13'd1;default:used<=used;
            endcase
            if(out_valid && out_ready) begin out_valid<=1'b0;if(out_last) done<=1'b1;end // 最后一个 AXIS 数据字被下游接受后，才置完成状态。
            if(pop) begin {out_last,out_keep,out_data}<=output_fifo[rd_ptr];rd_ptr<=rd_ptr+12'd1;out_valid<=1'b1;end
        end
    end
endmodule
module panorama_entropy_merge( // 按 MCU 的 Y/Cb/Cr 分量顺序合并，遇块尾可同拍跨分量取第二码字。
    input wire clk,rstn,input wire [34:0] h0_y,h1_y,h0_u,h1_u,h0_v,h1_v,input wire [9:0] count_y,count_u,count_v,
    output wire [1:0] pop_y,pop_u,pop_v,output wire [1:0] offered,output wire [34:0] first_token,second_token,input wire code_ready
);
    wire [34:0] h0[0:2],h1[0:2];wire [9:0] token_count[0:2];wire [1:0] pop_count[0:2];
    assign h0[0]=h0_y;assign h1[0]=h1_y;assign h0[1]=h0_u;assign h1[1]=h1_u;assign h0[2]=h0_v;assign h1[2]=h1_v;
    assign token_count[0]=count_y;assign token_count[1]=count_u;assign token_count[2]=count_v;
    assign pop_y=pop_count[0];assign pop_u=pop_count[1];assign pop_v=pop_count[2];
    reg [1:0] merge_component;wire [1:0] following_component;wire first_available,second_available;
    wire code_take;
    assign following_component=merge_component==2'd2 ? 2'd0 : merge_component+2'd1;
    assign first_token=h0[merge_component];assign first_available=token_count[merge_component]!=10'd0;
    assign second_token=first_token[33] ? h0[following_component] : h1[merge_component];
    assign second_available=first_token[33] ? token_count[following_component]!=10'd0 : token_count[merge_component]>=10'd2;
    assign offered=!first_available ? 2'd0 : second_available && !first_token[34] ? 2'd2 : 2'd1;
    assign code_take=offered!=2'd0 && code_ready;
    genvar component;generate for(component=0;component<3;component=component+1) begin:gen_pop
        assign pop_count[component]=!code_take ? 2'd0 : (merge_component==component ? 2'd1 : 2'd0)+
             (offered==2'd2 && (first_token[33] ? following_component : merge_component)==component ? 2'd1 : 2'd0);
    end endgenerate
    always @(posedge clk) begin
        if(!rstn) merge_component<=2'd0;
        else if(code_take) begin
            if(offered==2'd1) begin if(first_token[33]) merge_component<=following_component;end
            else if(first_token[33]) begin
                if(second_token[33]) merge_component<=following_component==2'd2 ? 2'd0 : following_component+2'd1;
                else merge_component<=following_component;
            end else if(second_token[33]) merge_component<=following_component;
        end
    end
endmodule
