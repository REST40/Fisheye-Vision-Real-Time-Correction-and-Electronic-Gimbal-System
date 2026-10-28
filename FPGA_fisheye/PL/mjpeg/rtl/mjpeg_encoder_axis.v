// 编码器标准流接口封装。
module mjpeg_encoder_axis #(
    parameter integer WIDTH = 2656,
    parameter integer HEIGHT = 1328
) (
    (* X_INTERFACE_PARAMETER = "ASSOCIATED_BUSIF s_axis:m_axis, ASSOCIATED_RESET aresetn" *)
    input  wire         aclk,

    input  wire         aresetn,

    input  wire [31:0]  s_axis_tdata,
    input  wire [3:0]   s_axis_tkeep,
    input  wire         s_axis_tlast,
    input  wire         s_axis_tvalid,
    output wire         s_axis_tready,

    output wire [31:0]  m_axis_tdata,
    output wire [3:0]   m_axis_tkeep,
    output wire         m_axis_tlast,
    output wire         m_axis_tvalid,
    input  wire         m_axis_tready,

    input  wire [31:0]  run,

    output wire [31:0]  status
);
    mjpeg_encoder_core #(.WIDTH(WIDTH), .HEIGHT(HEIGHT)) core (
        .aclk(aclk),
        .aresetn(aresetn),
        .s_axis_tdata(s_axis_tdata),
        .s_axis_tkeep(s_axis_tkeep),
        .s_axis_tlast(s_axis_tlast),
        .s_axis_tvalid(s_axis_tvalid),
        .s_axis_tready(s_axis_tready),
        .m_axis_tdata(m_axis_tdata),
        .m_axis_tkeep(m_axis_tkeep),
        .m_axis_tlast(m_axis_tlast),
        .m_axis_tvalid(m_axis_tvalid),
        .m_axis_tready(m_axis_tready),
        .run(run),
        .status(status)
    );
endmodule
