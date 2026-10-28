module fc_reader ( // 128 位 AXI 连续读：单 ID 顺序返回，最多四个突发在途。
    input wire clk, input wire rst_n, input wire abort,
    input wire cmd_valid, output wire cmd_ready,
    input wire [31:0] cmd_addr, input wire [19:0] cmd_beats, // 命令地址须 16 字节对齐，长度以 128 位数据拍计。
    output wire data_valid, input wire data_ready, output wire [127:0] data,
    output reg done, output reg error, output wire idle,
    output reg [31:0] araddr, output reg [7:0] arlen, output wire arvalid, input wire arready,
    input wire [127:0] rdata, input wire [1:0] rresp, input wire rlast, input wire rvalid, output wire rready
);
    reg busy, dropping, address_pending;
    reg [31:0] next_addr; reg [19:0] remaining;
    reg [8:0] address_beats, received;
    reg [8:0] burst_length [0:3];
    reg [1:0] burst_wp, burst_rp; reg [2:0] outstanding;
    reg [10:0] unreturned;
    wire [8:0] boundary_beats, chosen; wire [19:0] limited; // 每个突发最多 128 拍，且不得跨越 4 KB 边界。
    assign boundary_beats = 9'd256-{1'b0,next_addr[11:4]};
    assign limited = remaining < 20'd128 ? remaining : 20'd128;
    assign chosen = limited < {11'd0,boundary_beats} ? limited[8:0] : boundary_beats;
    wire fifo_valid, fifo_push, fifo_pop, fifo_clear; wire [10:0] fifo_level;
    wire address_take, response_take, response_end, bad_response, discard_now;
    wire [11:0] required_capacity; wire [8:0] credit_release; wire [32:0] command_end;
    assign required_capacity = {1'b0,fifo_level}+{1'b0,unreturned}+{3'd0,chosen}; // 发 AR 前，为 FIFO 已占用、未返回和新突发数据预留完整容量。
    assign command_end = {1'b0,cmd_addr}+{9'd0,cmd_beats,4'd0};
    assign idle = !busy; assign cmd_ready = !busy && !abort;
    assign arvalid = address_pending; assign address_take = address_pending && arready; // 已呈现的 ARVALID 和负载保持至握手，故障时也不能撤销。
    assign rready = busy && outstanding != 0 && (abort || dropping || fifo_level < 11'd512); // 故障排空持续接收在途响应，不受上游数据背压阻塞。
    assign response_take = rvalid && rready; assign response_end = response_take && rlast;
    assign bad_response = response_take && (rresp != 2'b00 || rlast != (received == burst_length[burst_rp]-1'b1)); // 逐拍检查 RRESP 和 RLAST；只有真实 RLAST 才弹出突发描述符。
    assign discard_now = abort || dropping || bad_response;
    assign data_valid = busy && fifo_valid && !discard_now;
    assign fifo_push = response_take && !discard_now; assign fifo_pop = data_valid && data_ready;
    assign fifo_clear = (cmd_valid && cmd_ready) || (!dropping && (abort || bad_response));
    assign credit_release = received >= burst_length[burst_rp] ? 9'd0 : rlast ? burst_length[burst_rp]-received : 9'd1; // 提前 RLAST 释放未返回尾拍的信用；超长响应不重复扣减。
    fc_fifo #(.WIDTH(128),.ADDR_BITS(9)) u_responses (
        .clk(clk),.rst_n(rst_n),.clear(fifo_clear),.push(fifo_push),.din(rdata),
        .pop(fifo_pop),.valid(fifo_valid),.dout(data),.level(fifo_level)
    );
    always @(posedge clk) begin
        if (!rst_n) begin // 系统复位须与互连同步；正常帧切换通过排空结束事务。
            busy <= 0; dropping <= 0; address_pending <= 0; next_addr <= 0; remaining <= 0;
            address_beats <= 0; received <= 0; burst_wp <= 0; burst_rp <= 0; outstanding <= 0; unreturned <= 0;
            araddr <= 0; arlen <= 0; done <= 0; error <= 0;
        end else begin
            done <= 0; error <= 0;
            if (cmd_valid && cmd_ready) begin
                next_addr <= cmd_addr; remaining <= cmd_beats; dropping <= 0; received <= 0;
                burst_wp <= 0; burst_rp <= 0; outstanding <= 0; unreturned <= 0; address_pending <= 0;
                if (cmd_addr[3:0] != 0 || cmd_beats == 0 || command_end[32]) begin busy <= 0; remaining <= 0; done <= 1; error <= 1; end
                else busy <= 1;
            end else if (busy) begin
                if (abort || bad_response) begin dropping <= 1; remaining <= 0; end // 故障后停止新增请求，丢弃缓存数据并排空已接受/已呈现的事务。
                if (!address_pending && !discard_now && remaining != 0 && outstanding < 3'd4 && required_capacity <= 12'd512) begin
                    araddr <= next_addr; arlen <= chosen[7:0]-1'b1; address_beats <= chosen; address_pending <= 1;
                end
                if (address_take) begin
                    address_pending <= 0; burst_length[burst_wp] <= address_beats; burst_wp <= burst_wp+1'b1;
                    next_addr <= next_addr+{19'd0,address_beats,4'd0};
                    if (!discard_now) remaining <= remaining-{11'd0,address_beats};
                end
                if (response_take) begin
                    if (bad_response) error <= 1;
                    if (rlast) begin received <= 0; burst_rp <= burst_rp+1'b1; end
                    else if (received < burst_length[burst_rp]) received <= received+1'b1;
                end
                case ({address_take,response_end})
                    2'b10: outstanding <= outstanding+1'b1;
                    2'b01: outstanding <= outstanding-1'b1;
                    default: outstanding <= outstanding;
                endcase
                case ({address_take,response_take})
                    2'b10: unreturned <= unreturned+{2'd0,address_beats};
                    2'b01: unreturned <= unreturned-{2'd0,credit_release};
                    2'b11: unreturned <= unreturned+{2'd0,address_beats}-{2'd0,credit_release};
                    default: unreturned <= unreturned;
                endcase
                if (remaining == 0 && !address_pending && outstanding == 0 && fifo_level == (fifo_pop ? 11'd1 : 11'd0)) begin busy <= 0; done <= 1; end // 返回响应和缓存数据均已排空后，才报告命令完成。
            end
        end
    end
endmodule