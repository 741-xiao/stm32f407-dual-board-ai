/**
  ******************************************************************************
  * @file    network_data_params.c
  * @author  AST Embedded Analytics Research Platform
  * @date    2026-10-12T19:09:44+0800
  * @brief   AI Tool Automatic Code Generator for Embedded NN computing
  ******************************************************************************
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  ******************************************************************************
  */

#include "network_data_params.h"


/**  Activations Section  ****************************************************/
ai_handle g_network_activations_table[1 + 2] = {
  AI_HANDLE_PTR(AI_MAGIC_MARKER),
  AI_HANDLE_PTR(NULL),
  AI_HANDLE_PTR(AI_MAGIC_MARKER),
};




/**  Weights Section  ********************************************************/
AI_ALIGNED(32)
const ai_u64 s_network_weights_array_u64[201] = {
  0x3dde28433ed866f4U, 0xbe14d4583d5676afU, 0xbe8f37edbf011c2dU, 0x3f26aa193e044b4aU,
  0xbea7953cbeb5c5b1U, 0xbee50cca3f314a63U, 0x3e9e954dbf075dbdU, 0xbe198fdbbf1192e4U,
  0xbef356f2be968d9cU, 0xbe087f3f3c8ca779U, 0x3f25bf27bed2770bU, 0xbe1e5ac73f228e81U,
  0xbeeebb26bd51f608U, 0xbe8148d33ca10bd7U, 0xbe9ad5203e805d95U, 0xbe58100c3f27ed89U,
  0x3cd72f63bf04c73fU, 0xbe1f5ea8be5a2398U, 0x3f45a0ef3eade839U, 0x3ed29585bf00b51eU,
  0x3efa5aecbe00e303U, 0x3eafc17dbea9e210U, 0xbefc2414be36a536U, 0x3f27c6753c4e1597U,
  0x3e652f0c3e70a40dU, 0xbe8e2a85beaf3cbcU, 0x3e83af33bdae058bU, 0x3f0d310bbd8602a6U,
  0xbd660782be4251bfU, 0xbe0ea2353eee23e3U, 0xbeeb5ad73eb48f74U, 0x3ed96c5a3d57610dU,
  0xbee8fd9fbea0d101U, 0x3f57a4273f0e3cc4U, 0x3f3722f0bf4fdb32U, 0x3ef920d2bebe009cU,
  0xbe30eaa63eeddc95U, 0xbe2869e43c792083U, 0x3f1b71bb3f12c8eaU, 0x3f0e597cbf1e4242U,
  0x3e82b6ea3d1cc2d7U, 0xbc0fe0353e8811b0U, 0xbe074ea23e870998U, 0x3e958c6d3e661a62U,
  0x3e7a730fbe41ea0fU, 0x3e86e1eabd52d232U, 0x3eaad7953da68913U, 0x3ecc1e1b3eab92b1U,
  0x3b57a200be1ff25cU, 0xbe9a298cbecbe044U, 0xbddcb4403e8c8095U, 0xbe3443bc3e4da082U,
  0x3d294a803e57c0eeU, 0x3ed4a3fd3eb21645U, 0xbe51a701beafdbcaU, 0xbeb2b582bd8d5698U,
  0x3b8828c0bcfa82d2U, 0x3d7008f0be81e327U, 0xbe7ac86e3e1beeaaU, 0xbe646afb3e5a4e9aU,
  0xbecf0f36beaa086aU, 0xbeb847e5be066aeaU, 0x3e197806bec5d685U, 0xbec5245fbee2f8ecU,
  0xbeb023663ceb13e0U, 0xbd54fb20be9a00ceU, 0xbea37030bec6f3b6U, 0xbeafe6f7be3eb613U,
  0xbed2f6c4be23ea12U, 0xbce0aa80be30f964U, 0xbe36b94ebe9e6044U, 0xbed308e63d696140U,
  0x3e4986d93e488757U, 0xbe446bd83ef79495U, 0x3e122b8bbc270495U, 0xbefbb0b23ea39f0bU,
  0xbdeb9ef33ecd2ceaU, 0x3f0c43a13e7704ccU, 0xbefcf0603eb84496U, 0xbe3f1cb0be8c7c8fU,
  0x3e9c992c3e931a95U, 0x3e04e06fbebd97cdU, 0x3d52472dbba77285U, 0x3ef46a0abe8db054U,
  0x3edc95093f081914U, 0x3ca34464bd30be44U, 0x3f0497443f06a92dU, 0x3f08d6833ecd6aa4U,
  0x3de12a373eb9529aU, 0x3e0b5e4c3f38a46dU, 0x3efcb780bebb0961U, 0xbe44383c3e291f2cU,
  0xbbef989f3d38adcaU, 0x3d8da592bc457c1aU, 0xbe8707eb3eebf79fU, 0x3e470586bec6f680U,
  0x3efafc5e3d1b88eeU, 0x3b91aaf93f1ad5e7U, 0xbc9bce12be95ec06U, 0xbeb4cd983eecc8aeU,
  0x3e6ff228be03e7ecU, 0x3f49c267be1357b0U, 0xbea2b7443e748408U, 0x3eb237e9bea124a9U,
  0x3f360cadbe32d998U, 0xbf09fd4dbd8d3fdfU, 0x3e900d473e9e88f3U, 0x3d6e6fd0bd02fa0cU,
  0x3eed51bfbec94eadU, 0x3e9f53563da58b4cU, 0x3f19904dbea6c0d1U, 0x3f13dcf03b289f57U,
  0x3e6a623dbf0ad4c3U, 0x3e504ce73e0d836bU, 0xbea39a4abe0a8350U, 0x3ef6c3c13e7a701dU,
  0xbe284daebf0d7823U, 0x3e973bfabef22bd3U, 0x3e5ceb833e607c88U, 0x3daed0d4be0404eaU,
  0xbe45634d3e8c3907U, 0x3d3e61333f12b09fU, 0xbf037edabdc8dbe7U, 0xbeb04677bd847aecU,
  0xbdfff670be036f64U, 0x3ed87233bd038dbdU, 0xbded83083f030b78U, 0xbcb5f514be306698U,
  0xbeee95b13e9b9fbeU, 0x3e0590d13ec1d799U, 0x3e8f9499bd79fe38U, 0x3e04c6463e19881aU,
  0xbe59492b3cd9334dU, 0xbe9356c73e21a161U, 0xbdfd3ddd3ee00579U, 0xbc76c99ebeeea5c1U,
  0xbe964a4abe0f3b03U, 0xbd0fc9a8be3613bcU, 0x3ddc4f50be23b7daU, 0xbe3ec530bedb1818U,
  0x3e021fc5be9c11f6U, 0x3dd59a5cbee5a5f6U, 0xbb667a0cbbc1d140U, 0x3d8e6c2d3e49fbbaU,
  0x3f1997603dc675a4U, 0x3e2809c0bec1624eU, 0xbc8d63b63f16f4b5U, 0x3f35dd853db7ddf1U,
  0x3f2c8eb33be03751U, 0xbd97fe7dbeeecb34U, 0xbd5e4af7be284305U, 0x3d80c60b3ea6be78U,
  0xbe1914aebe2d3450U, 0xbe91c149beceef73U, 0x3e43ac363ea6e0a1U, 0xbecc4db9be5a4881U,
  0xbe5c6e0d3ecdeeb7U, 0xbd283c303e45702aU, 0xbe93f6a0bec7660aU, 0x3d148e183c8bd590U,
  0x3e921b2abe76127dU, 0xbd5839543eda60b7U, 0xbebbd1d73c8ba550U, 0xbd6a29c0be6dacd4U,
  0xbea50f65beab5c75U, 0x40111e223e0fda12U, 0xbec16d773f261017U, 0x3ecf8f2dbea29588U,
  0xbe3a53523f10e9feU, 0x3ef2b2043f0ea6bfU, 0xbcdcdbd9bed99000U, 0xbe0bd4613ed24180U,
  0xbe9161f53c7577c6U, 0x3e5c47493eae6402U, 0x3d95b40a3ed8abddU, 0xbdb2932ebe399f03U,
  0xbd8d052e00000000U, 0x3e877cc500000000U, 0x3e8a03cc3e744386U, 0x3e62cde43e6f56ceU,
  0x3d72b2eebdbbe820U, 0xbd75f69ebdb4de1aU, 0x3e2b2e9bU, 0x3e69fbef3bf2b6c5U,
  0x3d9b1af7bdbcc674U, 0x3d174ce9bf12172cU, 0xbcdd17ae3ea06881U, 0x3e06aae8bed1c55eU,
  0xbdc8b8a2bd508357U, 0xbea0e6f8bea5dbf9U, 0xbe995bd63f1d195aU, 0xbf1095ecbf241e08U,
  0x3e21a83e3ecb6954U, 0x3f36e7c13e81daa4U, 0x3f1f4a26bf3509ebU, 0xbf66daeb3f3aaa02U,
  0x3f20aac7bd906bf1U, 0xbac12f8c3e0ea27aU, 0x3e4b0bf8bf63fdfcU, 0x3edc38463f48952bU,
  0x3d8f40ccbd9ed05fU,
};


ai_handle g_network_weights_table[1 + 2] = {
  AI_HANDLE_PTR(AI_MAGIC_MARKER),
  AI_HANDLE_PTR(s_network_weights_array_u64),
  AI_HANDLE_PTR(AI_MAGIC_MARKER),
};

