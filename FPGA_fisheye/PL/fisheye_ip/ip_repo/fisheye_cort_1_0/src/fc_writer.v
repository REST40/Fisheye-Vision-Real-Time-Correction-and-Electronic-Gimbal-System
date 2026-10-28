module fc_writer ( // 输出 FIFO 驱动的 128 位 AXI 写引擎，按 AW、W、B 顺序完成突发。
    input wire clk, input wire rst_n, input wire start, input wire abort,
    input wire [31:0] base, input wire [19:0] total_beats,
    input wire [10:0] level, input wire valid, input wire [127:0] data, output wire pop,
    output reg done, output reg error, output wire idle,
    output reg [31:0] awaddr, output reg [7:0] awlen, output wire awvalid, input wire awready,
    output wire [127:0] wdata, output wire wvalid, input wire wready, output wire wlast,
    input wire [1:0] bresp, input wire bvalid, output wire bready
);
    localparam IDLE=3'd0, WAIT_DATA=3'd1, ADDR=3'd2, DATA=3'd3, RESP=3'd4;
    reg [2:0] state;
    reg [31:0] next_addr;
    reg [19:0] remaining;
    reg [8:0] burst_beats, sent;
    wire [8:0] boundary_beats; // 单突发最多 128 拍，并按 4 KB 边界拆分。
    assign boundary_beats = 9'd256-{1'b0,next_addr[11:4]};
    wire [19:0] limit_beats;
    assign limit_beats = remaining < 20'd128 ? remaining : 20'd128;
    wire [8:0] chosen;
    assign chosen = limit_beats < {11'd0,boundary_beats} ? limit_beats[8:0] : boundary_beats;
    assign idle = state == IDLE;
    assign awvalid = state == ADDR;
    assign wvalid = (state == DATA) && valid;
    assign wdata = data;
    assign wlast = sent == burst_beats-1'b1;
    assign pop = wvalid && wready; // 仅在 W 通道握手后释放 FIFO 队首。
    assign bready = state == RESP;
    always @(posedge clk) begin
        if (!rst_n) begin
            state <= IDLE; next_addr <= 0; remaining <= 0; burst_beats <= 0; sent <= 0;
            awaddr <= 0; awlen <= 0; done <= 0; error <= 0;
        end else begin
            done <= 0; error <= 0; // done/error 为单周期脉冲，由顶层锁存状态。
            case (state)
                IDLE: if (start) begin next_addr <= base; remaining <= total_beats; state <= WAIT_DATA; end
                WAIT_DATA: begin // 完整突发数据齐备后才呈现 AW，防止提交后缺少写数据。
                    if (abort) state <= IDLE;
                    else if (level >= {2'd0,chosen}) begin
                        awaddr <= next_addr; awlen <= chosen[7:0]-1'b1;
                        burst_beats <= chosen; sent <= 0; state <= ADDR;
                    end
                end
                ADDR: if (awready) state <= DATA; // 已呈现的写事务须完成 AW/W/B；abort 仅停止后续突发。
                DATA: if (pop) begin
                    sent <= sent + 1'b1;
                    if (wlast) state <= RESP;
                end
                RESP: if (bvalid) begin
                    if (bresp != 0) error <= 1;
                    next_addr <= next_addr + {19'd0,burst_beats,4'd0};
                    remaining <= remaining - burst_beats;
                    if (abort || bresp != 0) state <= IDLE;
                    else if (remaining == {11'd0,burst_beats}) begin state <= IDLE; done <= 1; end // 最后一个 B 响应成功返回后，才报告整帧写回完成。
                    else state <= WAIT_DATA;
                end
                default: state <= IDLE;
            endcase
        end
    end
endmodule
