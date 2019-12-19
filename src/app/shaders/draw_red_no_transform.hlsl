/*
 Copyright 2018 Google Inc.

 Licensed under the Apache License, Version 2.0 (the "License");
 you may not use this file except in compliance with the License.
 You may obtain a copy of the License at

 http://www.apache.org/licenses/LICENSE-2.0

 Unless required by applicable law or agreed to in writing, software
 distributed under the License is distributed on an "AS IS" BASIS,
 WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 See the License for the specific language governing permissions and
 limitations under the License.
*/

struct VSInput {
  float3 Position : Position;
};

struct VSOutput {
  float4 Position : SV_Position;
};

VSOutput vsmain(VSInput input)
{
  VSOutput output = (VSOutput)0;
  output.Position = float4(input.Position, 1);
  return output;
}

struct PSInput {
  float4 Position : SV_Position;
};

float4 psmain(PSInput input) : SV_Target
{
  return float4(1, 0, 0, 1);
}
