`timescale 1ns/1ps
// 两路 RGBX AXIS 选择：s0 为 DMA MM2S，s1 为校正器直连像素流。
module axis_stream_mux (
    input wire aclk, input wire aresetn, input wire [31:0] control,
    input wire [31:0] s0_axis_tdata, input wire [3:0] s0_axis_tkeep, input wire s0_axis_tlast, input wire s0_axis_tvalid, output wire s0_axis_tready,
    input wire [31:0] s1_axis_tdata, input wire [3:0] s1_axis_tkeep, input wire s1_axis_tlast, input wire s1_axis_tvalid, output wire s1_axis_tready,
    output wire [31:0] m_axis_tdata, output wire [3:0] m_axis_tkeep, output wire m_axis_tlast, output wire m_axis_tvalid, input wire m_axis_tready
);
    reg select_stream;
    always @(posedge aclk) begin
        if (!aresetn) select_stream <= 0;
        else if (!control[0]) select_stream <= control[23]; // control[0]=0 时锁存 control[23]，一帧运行期间保持输入源不变。
    end
    assign m_axis_tdata = select_stream ? s1_axis_tdata : s0_axis_tdata;
    assign m_axis_tkeep = select_stream ? s1_axis_tkeep : s0_axis_tkeep;
    assign m_axis_tlast = select_stream ? s1_axis_tlast : s0_axis_tlast;
    assign m_axis_tvalid = select_stream ? s1_axis_tvalid : s0_axis_tvalid;
    assign s0_axis_tready = !select_stream && m_axis_tready; // 仅向被选择的输入传播 TREADY。
    assign s1_axis_tready = select_stream && m_axis_tready;
endmodule
