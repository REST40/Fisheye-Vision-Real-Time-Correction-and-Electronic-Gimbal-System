module fc_fifo #(parameter WIDTH=128, parameter ADDR_BITS=9) ( // 同步 BRAM 弹性队列，使用预取寄存器保持队首在背压期间稳定。
    input wire clk, input wire rst_n, input wire clear,
    input wire push, input wire [WIDTH-1:0] din,
    input wire pop, output wire valid, output wire [WIDTH-1:0] dout,
    output wire [ADDR_BITS+1:0] level
);
    (* ram_style="block" *) reg [WIDTH-1:0] mem [0:(1<<ADDR_BITS)-1];
    reg [ADDR_BITS-1:0] wp, rp;
    reg [ADDR_BITS:0] count;
    reg [WIDTH-1:0] q;
    reg q_valid;
    wire fetch;
    assign fetch = (!q_valid || pop) && (count != 0);
    assign valid = q_valid;
    assign dout = q;
    assign level = {1'b0,count} + q_valid; // 占用量包括 BRAM 内数据和已预取的队首，供上游预留容量。
    always @(posedge clk) begin // 仅复位指针和有效位，不复位 RAM 阵列，以保留 BRAM 推断。
        if (push) mem[wp] <= din;
        if (fetch) q <= mem[rp];
    end
    always @(posedge clk) begin
        if (!rst_n) begin
            wp <= 0; rp <= 0; count <= 0; q_valid <= 0;
        end else if (clear) begin
            wp <= 0; rp <= 0; count <= 0; q_valid <= 0;
        end else begin
            if (push) wp <= wp + 1'b1;
            if (fetch) rp <= rp + 1'b1;
            case ({push,fetch})
                2'b10: count <= count + 1'b1;
                2'b01: count <= count - 1'b1;
                default: count <= count;
            endcase
            if (fetch || pop) q_valid <= fetch; // 消费且无新预取时撤销有效位；同时预取时保持连续输出。
        end
    end
endmodule
