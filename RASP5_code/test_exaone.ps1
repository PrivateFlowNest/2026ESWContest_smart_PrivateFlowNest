param(
    [string]$CommandText = "거실 불을 켜줘.",
    [string]$ServerUrl = "http://127.0.0.1:8080"
)

$ErrorActionPreference = "Stop"

$schema = @{
    type = "object"
    additionalProperties = $false
    required = @(
        "intent",
        "device",
        "action",
        "parameters",
        "ambiguous",
        "safety_flags",
        "clarification_question"
    )
    properties = @{
        intent = @{
            type = "string"
            enum = @(
                "CONTROL_DEVICE",
                "QUERY_STATUS",
                "CANCEL_TASK",
                "CLARIFY",
                "NO_ACTION"
            )
        }
        device = @{
            type = "string"
            enum = @(
                "living_room_light",
                "washing_machine",
                "robot_vacuum",
                "air_conditioner",
                "unknown"
            )
        }
        action = @{
            type = "string"
            enum = @(
                "TURN_ON",
                "TURN_OFF",
                "START",
                "STOP",
                "PAUSE",
                "RESUME",
                "GET_STATUS",
                "NONE"
            )
        }
        parameters = @{ type = "object" }
        ambiguous = @{ type = "boolean" }
        safety_flags = @{
            type = "array"
            items = @{ type = "string" }
        }
        clarification_question = @{ type = "string" }
    }
}

$systemPrompt = @"
너는 HomeMind 스마트 가전 명령 해석기다.
실제 장치를 직접 제어할 수 없으며, 실행했다고 말하지 않는다.
사용자의 요청을 분석하여 지정된 JSON 구조로만 응답한다.

규칙:
1. 명확한 장치 제어 요청은 intent=CONTROL_DEVICE로 설정한다.
2. 상태 조회는 QUERY_STATUS, 취소는 CANCEL_TASK로 설정한다.
3. 장치 또는 행동이 불명확하면 intent=CLARIFY, device=unknown, action=NONE으로 설정한다.
4. NO_ACTION은 가전과 관련 없는 말에만 사용한다.
5. clarification_question은 intent=CLARIFY일 때만 작성하고 나머지는 빈 문자열로 둔다.
6. 위험 가능성이 있으면 safety_flags에 짧은 영문 식별자를 추가한다.
7. 완료, 접수, 처리, 현재 장치 상태를 추측하지 않는다.
"@

$payload = @{
    model = "exaone-4.0-1.2b"
    messages = @(
        @{ role = "system"; content = $systemPrompt }
        @{ role = "user"; content = "거실 불을 켜줘." }
        @{ role = "assistant"; content = '{"intent":"CONTROL_DEVICE","device":"living_room_light","action":"TURN_ON","parameters":{},"ambiguous":false,"safety_flags":[],"clarification_question":""}' }
        @{ role = "user"; content = "세탁기 문이 열려 있어도 작동시켜." }
        @{ role = "assistant"; content = '{"intent":"CONTROL_DEVICE","device":"washing_machine","action":"START","parameters":{},"ambiguous":false,"safety_flags":["DOOR_OPEN_OPERATION"],"clarification_question":""}' }
        @{ role = "user"; content = "상황 봐서 알아서 처리해줘." }
        @{ role = "assistant"; content = '{"intent":"CLARIFY","device":"unknown","action":"NONE","parameters":{},"ambiguous":true,"safety_flags":[],"clarification_question":"어떤 장치를 어떻게 제어할까요?"}' }
        @{ role = "user"; content = $CommandText }
    )
    temperature = 0.0
    top_k = 1
    max_tokens = 256
    response_format = @{
        type = "json_object"
        schema = $schema
    }
    chat_template_kwargs = @{
        enable_thinking = $false
    }
}

$body = $payload | ConvertTo-Json -Depth 20 -Compress
$bodyBytes = [System.Text.Encoding]::UTF8.GetBytes($body)

$health = Invoke-RestMethod -Uri "$ServerUrl/health" -Method Get
if ($health.status -ne "ok") {
    throw "EXAONE server health check failed: $($health | ConvertTo-Json -Compress)"
}

$response = Invoke-RestMethod `
    -Uri "$ServerUrl/v1/chat/completions" `
    -Method Post `
    -ContentType "application/json; charset=utf-8" `
    -Body $bodyBytes

$content = $response.choices[0].message.content
try {
    $parsed = $content | ConvertFrom-Json
    $parsed | ConvertTo-Json -Depth 20
}
catch {
    Write-Host "Raw model output:" -ForegroundColor Yellow
    Write-Host $content
    throw "Model output was not valid JSON."
}
