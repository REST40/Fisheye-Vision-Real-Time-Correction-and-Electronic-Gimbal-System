// RGBX 写回：每组八像素、32 组缓冲，最多八个地址等待 B 响应。
module mjpeg_rgb_burst (
    input wire clk,rst,clear,abort, // clear 仅用于旧事务排空后的新帧初始化。
    input wire pixel_valid, output wire pixel_ready, input wire [31:0] pixel_addr,pixel_data,
    output wire awvalid, input wire awready, output wire [31:0] awaddr, output wire [7:0] awlen,
    output wire wvalid, input wire wready, output wire [31:0] wdata, output wire wlast,
    input wire bvalid, input wire [1:0] bresp, output wire bready,
    output wire idle, output reg error, output wire [3:0] pending_writes
);
    (* ram_style="distributed" *) reg [31:0] data_mem [0:255];
    (* ram_style="distributed" *) reg [31:0] addr_mem [0:31];
    reg [4:0] wr_group,aw_group,rd_group; reg [2:0] wr_pixel,rd_pixel;
    reg [5:0] groups; reg [3:0] issued_groups,outstanding; // groups 计完整数据组；issued_groups 等待 W，outstanding 等待 B。
    reg address_pending,dropping;
    wire pixel_take,commit_group,address_take,data_take,retire_group,response_take;
    wire bad_pixel,bad_response,stop_now;
    assign pixel_ready=!abort && !dropping && !error && groups<6'd32;
    assign pixel_take=pixel_valid && pixel_ready;
    assign bad_pixel=pixel_take && (wr_pixel==3'd0 ? pixel_addr[4:0]!=5'd0 : pixel_addr!=addr_mem[wr_group]+{27'd0,wr_pixel,2'd0}); // 组首地址 32 字节对齐，组内八个 RGBX 地址必须连续。
    assign commit_group=pixel_take && wr_pixel==3'd7 && !bad_pixel;
    assign awvalid=address_pending; assign awaddr=addr_mem[aw_group]; assign awlen=8'd7;
    assign address_take=awvalid && awready;
    assign wvalid=issued_groups!=4'd0; assign wdata=data_mem[{rd_group,rd_pixel}]; assign wlast=rd_pixel==3'd7;
    assign data_take=wvalid && wready; assign retire_group=data_take && wlast;
    assign bready=outstanding!=4'd0 && (outstanding>issued_groups || retire_group); // B 仅对应已发送完 W 的事务；允许末 W 与 B 同拍完成。
    assign response_take=bvalid && bready; assign bad_response=response_take && bresp!=2'b00;
    assign stop_now=abort || dropping || error || bad_response || bad_pixel; // 故障停止新增像素和地址，但已提交的 AW/W/B 仍须排空。
    assign pending_writes=outstanding;
    assign idle=!address_pending && issued_groups==4'd0 && outstanding==4'd0 && (dropping || (groups==6'd0 && wr_pixel==3'd0)); // 正常空闲要求无部分像素组；故障丢弃未提交组，待下一帧 clear。
    always @(posedge clk) begin
        if(pixel_take && !bad_pixel) begin
            data_mem[{wr_group,wr_pixel}]<=pixel_data;
            if(wr_pixel==3'd0) addr_mem[wr_group]<=pixel_addr;
        end
    end
    always @(posedge clk) begin
        if (rst) begin
            wr_group<=5'd0;aw_group<=5'd0;rd_group<=5'd0;wr_pixel<=3'd0;rd_pixel<=3'd0;
            groups<=6'd0;issued_groups<=4'd0;outstanding<=4'd0;address_pending<=1'b0;dropping<=1'b0;error<=1'b0;
        end else if(clear) begin
            wr_group<=5'd0;aw_group<=5'd0;rd_group<=5'd0;wr_pixel<=3'd0;rd_pixel<=3'd0;
            groups<=6'd0;issued_groups<=4'd0;outstanding<=4'd0;address_pending<=1'b0;dropping<=1'b0;error<=1'b0;
        end else begin
            if(stop_now) dropping<=1'b1;
            if(bad_pixel || bad_response) error<=1'b1;
            if(pixel_take && !bad_pixel) begin
                wr_pixel<=wr_pixel+3'd1;
                if(commit_group) wr_group<=wr_group+5'd1;
            end
            if(!address_pending && !stop_now && groups>{2'd0,issued_groups} && outstanding<4'd8) address_pending<=1'b1; // 仅提交完整组，AW 负载保持至握手且受八在途额度限制。
            if(address_take) begin address_pending<=1'b0;aw_group<=aw_group+5'd1;end
            if(data_take) begin
                rd_pixel<=rd_pixel+3'd1;
                if(retire_group) rd_group<=rd_group+5'd1;
            end
            case({commit_group,retire_group})
                2'b10:groups<=groups+6'd1;
                2'b01:groups<=groups-6'd1;
                default:groups<=groups;
            endcase
            case({address_take,retire_group})
                2'b10:issued_groups<=issued_groups+4'd1;
                2'b01:issued_groups<=issued_groups-4'd1;
                default:issued_groups<=issued_groups;
            endcase
            case({address_take,response_take})
                2'b10:outstanding<=outstanding+4'd1;
                2'b01:outstanding<=outstanding-4'd1;
                default:outstanding<=outstanding;
            endcase
        end
    end
endmodule

module mjpeg_jpeg_prefetch #(parameter [3:0] AXI_ID=4'd0) ( // JPEG 预取：两个 512 字 BRAM 槽，最多两个短读突发在途。
    input wire clk,rst,start,stop, input wire [31:0] src_addr, input wire [23:0] byte_length,
    output wire data_valid, input wire data_ready, output wire [31:0] data, output wire [10:0] level,
    output reg [31:0] araddr, output reg [7:0] arlen, output wire arvalid, input wire arready,
    input wire [31:0] rdata, input wire [1:0] rresp, input wire [3:0] rid, input wire rlast,rvalid, output wire rready,
    output wire idle, output reg error, output wire [1:0] pending_reads, output wire [31:0] bytes_unissued
);
    (* ram_style="block" *) reg [31:0] words [0:1023];
    reg active,dropping,fill_active,fill_bank,read_bank,address_pending;
    reg [1:0] bank_ready; reg [9:0] bank_words [0:1];
    reg [9:0] fill_words,issued_words,received_words; reg [8:0] read_index;
    reg [23:0] words_left; reg [31:0] next_addr; reg [3:0] address_words,burst_received;
    reg [3:0] burst_words [0:1]; reg burst_wp,burst_rp; reg [1:0] outstanding;
    reg q_valid; reg [31:0] q_word;
    wire [24:0] rounded_length; wire [22:0] total_words; wire [32:0] source_end;
    assign rounded_length={1'b0,byte_length}+25'd3;assign total_words=rounded_length[24:2]; // 长度按四字节向上取整，同时检查读范围未溢出 32 位地址。
    assign source_end={1'b0,src_addr}+{8'd0,total_words,2'd0};
    wire [9:0] fill_remaining; wire [3:0] chosen; wire address_take,response_take,response_end,bad_response,stop_now,pop;
    assign fill_remaining=fill_words-issued_words;
    assign chosen=fill_remaining>=10'd8 && next_addr[4:0]==5'd0 ? 4'd8 : 4'd1; // 32 字节对齐时读八字突发，否则单字读取，不跨 4 KB 边界。
    assign arvalid=address_pending;assign address_take=arvalid && arready; // 停止后仍须完成已呈现 AR 的握手。
    assign rready=active && outstanding!=2'd0;assign response_take=rvalid && rready;assign response_end=response_take && rlast;
    assign bad_response=response_take && (rresp!=2'b00 || rid!=AXI_ID || rlast!=(burst_received==burst_words[burst_rp]-4'd1)); // 单 ID 顺序响应，逐拍检查响应码、ID 和 RLAST 位置。
    assign stop_now=stop || dropping || error || bad_response;
    assign data_valid=active && q_valid && !stop_now;assign data=q_word;assign pop=data_valid && data_ready;
    assign level=(bank_ready[0] ? {1'b0,bank_words[0]}-(read_bank ? 11'd0 : {2'd0,read_index}) : 11'd0)+(bank_ready[1] ? {1'b0,bank_words[1]}-(read_bank ? {2'd0,read_index} : 11'd0) : 11'd0);
    assign pending_reads=outstanding;assign bytes_unissued={6'd0,words_left,2'd0};
    assign idle=!active && !address_pending && outstanding==2'd0;
    always @(posedge clk) begin
        if(response_take && !stop_now) words[{fill_bank,received_words[8:0]}]<=rdata;
    end
    always @(posedge clk) begin
        if (rst) begin
            active<=1'b0;dropping<=1'b0;fill_active<=1'b0;fill_bank<=1'b0;read_bank<=1'b0;address_pending<=1'b0;
            bank_ready<=2'b00;bank_words[0]<=10'd0;bank_words[1]<=10'd0;fill_words<=10'd0;issued_words<=10'd0;received_words<=10'd0;read_index<=9'd0;
            words_left<=24'd0;next_addr<=32'd0;address_words<=4'd0;burst_received<=4'd0;
            burst_wp<=1'b0;burst_rp<=1'b0;outstanding<=2'd0;q_valid<=1'b0;q_word<=32'd0;error<=1'b0;araddr<=32'd0;arlen<=8'd0;
        end else if(start) begin // 仅在旧请求排空后允许 start，避免迟到响应进入新帧。
            active<=start;dropping<=1'b0;fill_active<=1'b0;fill_bank<=1'b0;read_bank<=1'b0;address_pending<=1'b0;
            bank_ready<=2'b00;bank_words[0]<=10'd0;bank_words[1]<=10'd0;fill_words<=10'd0;issued_words<=10'd0;received_words<=10'd0;read_index<=9'd0;
            words_left<=start ? {1'b0,total_words} : 24'd0;next_addr<=start ? src_addr : 32'd0;address_words<=4'd0;burst_received<=4'd0;
            burst_wp<=1'b0;burst_rp<=1'b0;outstanding<=2'd0;q_valid<=1'b0;q_word<=32'd0;error<=1'b0;araddr<=32'd0;arlen<=8'd0;
            if(start && (byte_length==24'd0 || src_addr[1:0]!=2'd0 || source_end[32])) begin active<=1'b0;words_left<=24'd0;error<=1'b1;end
        end else if(active) begin
            if(stop_now) begin dropping<=1'b1;words_left<=24'd0;bank_ready<=2'b00;q_valid<=1'b0;end
            if(bad_response) error<=1'b1;
            if(!fill_active && !stop_now && words_left!=24'd0 && !bank_ready[fill_bank]) begin
                fill_active<=1'b1;fill_words<=words_left<24'd512 ? words_left[9:0] : 10'd512;
                bank_words[fill_bank]<=words_left<24'd512 ? words_left[9:0] : 10'd512;issued_words<=10'd0;received_words<=10'd0;
            end
            if(fill_active && !address_pending && !stop_now && issued_words<fill_words && outstanding<2'd2) begin
                araddr<=next_addr;arlen<={4'd0,chosen}-8'd1;address_words<=chosen;address_pending<=1'b1;
            end
            if(address_take) begin
                address_pending<=1'b0;burst_words[burst_wp]<=address_words;burst_wp<=!burst_wp;
                next_addr<=next_addr+{26'd0,address_words,2'd0};issued_words<=issued_words+{6'd0,address_words};
                if(!stop_now) words_left<=words_left-{20'd0,address_words};
            end
            if(response_take) begin
                if(rlast) begin burst_received<=4'd0;burst_rp<=!burst_rp;end else if(burst_received<burst_words[burst_rp]) burst_received<=burst_received+4'd1;
                if(!stop_now) begin
                    received_words<=received_words+10'd1;
                    if(received_words+10'd1==fill_words) begin bank_ready[fill_bank]<=1'b1;fill_active<=1'b0;fill_bank<=!fill_bank;end // 整槽响应全部验证成功后才发布 bank_ready，填充与消费使用不同槽。
                end
            end
            case({address_take,response_end})
                2'b10:outstanding<=outstanding+2'd1;
                2'b01:outstanding<=outstanding-2'd1;
                default:outstanding<=outstanding;
            endcase
            if(!stop_now && bank_ready[read_bank]) begin // 同步预取保持队首稳定，握手后才前进；消费尾字时释放槽。
                if(!q_valid) begin q_word<=words[{read_bank,read_index}];q_valid<=1'b1;end
                else if(pop) begin
                    if({1'b0,read_index}+10'd1==bank_words[read_bank]) begin bank_ready[read_bank]<=1'b0;read_bank<=!read_bank;read_index<=9'd0;q_valid<=1'b0;end
                    else begin read_index<=read_index+9'd1;q_word<=words[{read_bank,read_index+9'd1}];end
                end
            end
            if(stop_now && !address_pending && outstanding==2'd0) begin active<=1'b0;fill_active<=1'b0;end // 停止或故障后，等待已呈现及在途请求排空才报告 idle。
            else if(!stop_now && words_left==24'd0 && !fill_active && !address_pending && outstanding==2'd0 && bank_ready==2'b00 && !q_valid) active<=1'b0;
        end
    end
endmodule