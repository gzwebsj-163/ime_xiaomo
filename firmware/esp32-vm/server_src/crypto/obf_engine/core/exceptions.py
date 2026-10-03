# -*- coding: utf-8 -*-
"""
OBF 异常定义模块

定义所有OBF相关的异常类：
- OBFError: 基础异常类
- ObfuscationError: 混淆过程异常
- DecryptionError: 解密异常
- VMError: 虚拟机异常
- InferenceError: 推理异常
- ConfigurationError: 配置异常
"""

from typing import Any, Dict, Optional


class OBFError(Exception):
    """
    OBF 基础异常类
    
    所有OBF相关异常的基类。
    
    Attributes:
        message: 错误消息
        error_code: 错误代码
        details: 详细信息
    
    Example:
        >>> try:
        ...     raise OBFError("操作失败", error_code="E001")
        ... except OBFError as e:
        ...     print(f"错误: {e.message}, 代码: {e.error_code}")
    """
    
    def __init__(
        self,
        message: str,
        error_code: Optional[str] = None,
        details: Optional[Dict[str, Any]] = None
    ):
        """
        初始化异常
        
        Args:
            message: 错误消息
            error_code: 错误代码，用于标识特定错误类型
            details: 错误详细信息字典
        """
        super().__init__(message)
        self.message = message
        self.error_code = error_code or "OBF_ERROR"
        self.details = details or {}
    
    def __str__(self) -> str:
        base = f"[{self.error_code}] {self.message}"
        if self.details:
            details_str = ", ".join(f"{k}={v}" for k, v in self.details.items())
            return f"{base} ({details_str})"
        return base
    
    def to_dict(self) -> Dict[str, Any]:
        """
        将异常转换为字典
        
        Returns:
            Dict[str, Any]: 异常信息字典
        """
        return {
            "error": self.error_code,
            "message": self.message,
            "details": self.details
        }


class ObfuscationError(OBFError):
    """
    混淆过程异常
    
    在混淆过程中发生的错误，例如：
    - 数据编码失败
    - 算术变换错误
    - 控制流生成失败
    
    Attributes:
        layer_id: 发生错误的层ID
        operation: 具体的操作名称
    """
    
    def __init__(
        self,
        message: str,
        layer_id: Optional[int] = None,
        operation: Optional[str] = None,
        error_code: Optional[str] = None,
        details: Optional[Dict[str, Any]] = None
    ):
        details = details or {}
        if layer_id is not None:
            details["layer_id"] = layer_id
        if operation:
            details["operation"] = operation
        
        super().__init__(
            message,
            error_code=error_code or "OBFUSCATION_ERROR",
            details=details
        )
        self.layer_id = layer_id
        self.operation = operation


class DecryptionError(OBFError):
    """
    解密异常
    
    在解密过程中发生的错误，例如：
    - 解密密钥错误
    - 数据格式不正确
    - 解密算法失败
    
    Attributes:
        algorithm: 使用的解密算法
        data_size: 数据大小
    """
    
    def __init__(
        self,
        message: str,
        algorithm: Optional[str] = None,
        data_size: Optional[int] = None,
        error_code: Optional[str] = None,
        details: Optional[Dict[str, Any]] = None
    ):
        details = details or {}
        if algorithm:
            details["algorithm"] = algorithm
        if data_size is not None:
            details["data_size"] = data_size
        
        super().__init__(
            message,
            error_code=error_code or "DECRYPTION_ERROR",
            details=details
        )
        self.algorithm = algorithm
        self.data_size = data_size


class VMError(OBFError):
    """
    虚拟机异常
    
    在虚拟机保护层发生的错误，例如：
    - 字节码生成失败
    - 虚拟机执行错误
    - 指令解析失败
    
    Attributes:
        vm_state: 虚拟机状态
        instruction: 当前指令
    """
    
    def __init__(
        self,
        message: str,
        vm_state: Optional[str] = None,
        instruction: Optional[str] = None,
        error_code: Optional[str] = None,
        details: Optional[Dict[str, Any]] = None
    ):
        details = details or {}
        if vm_state:
            details["vm_state"] = vm_state
        if instruction:
            details["instruction"] = instruction
        
        super().__init__(
            message,
            error_code=error_code or "VM_ERROR",
            details=details
        )
        self.vm_state = vm_state
        self.instruction = instruction


class InferenceError(OBFError):
    """
    推理异常
    
    在推理过程中发生的错误，例如：
    - 模型加载失败
    - 推理执行错误
    - 内存不足
    
    Attributes:
        model_name: 模型名称
        stage: 推理阶段
    """
    
    def __init__(
        self,
        message: str,
        model_name: Optional[str] = None,
        stage: Optional[str] = None,
        error_code: Optional[str] = None,
        details: Optional[Dict[str, Any]] = None
    ):
        details = details or {}
        if model_name:
            details["model_name"] = model_name
        if stage:
            details["stage"] = stage
        
        super().__init__(
            message,
            error_code=error_code or "INFERENCE_ERROR",
            details=details
        )
        self.model_name = model_name
        self.stage = stage


class ConfigurationError(OBFError):
    """
    配置异常

    配置相关的错误，例如：
    - 配置文件格式错误
    - 配置参数无效
    - 必要配置缺失

    Attributes:
        config_key: 配置键
        config_value: 配置值
    """

    def __init__(
        self,
        message: str,
        config_key: Optional[str] = None,
        config_value: Optional[Any] = None,
        error_code: Optional[str] = None,
        details: Optional[Dict[str, Any]] = None
    ):
        details = details or {}
        if config_key:
            details["config_key"] = config_key
        if config_value is not None:
            details["config_value"] = str(config_value)

        super().__init__(
            message,
            error_code=error_code or "CONFIGURATION_ERROR",
            details=details
        )
        self.config_key = config_key
        self.config_value = config_value


class CryptoError(OBFError):
    """
    加密异常

    在加密/解密过程中发生的错误，例如：
    - 加密算法失败
    - 密钥管理错误
    - 数据完整性验证失败

    Attributes:
        algorithm: 加密算法
        operation: 操作类型（encrypt/decrypt）
    """

    def __init__(
        self,
        message: str,
        algorithm: Optional[str] = None,
        operation: Optional[str] = None,
        error_code: Optional[str] = None,
        details: Optional[Dict[str, Any]] = None
    ):
        details = details or {}
        if algorithm:
            details["algorithm"] = algorithm
        if operation:
            details["operation"] = operation

        super().__init__(
            message,
            error_code=error_code or "CRYPTO_ERROR",
            details=details
        )
        self.algorithm = algorithm
        self.operation = operation


class ModelLoadError(OBFError):
    """
    模型加载异常

    在模型加载过程中发生的错误，例如：
    - 模型文件不存在
    - 模型格式不支持
    - 模型完整性验证失败

    Attributes:
        model_path: 模型路径
        model_type: 模型类型
    """

    def __init__(
        self,
        message: str,
        model_path: Optional[str] = None,
        model_type: Optional[str] = None,
        error_code: Optional[str] = None,
        details: Optional[Dict[str, Any]] = None
    ):
        details = details or {}
        if model_path:
            details["model_path"] = model_path
        if model_type:
            details["model_type"] = model_type

        super().__init__(
            message,
            error_code=error_code or "MODEL_LOAD_ERROR",
            details=details
        )
        self.model_path = model_path
        self.model_type = model_type


# 错误代码常量定义
class ErrorCodes:
    """错误代码常量"""
    
    # 通用错误
    UNKNOWN = "OBF_UNKNOWN"
    INTERNAL_ERROR = "OBF_INTERNAL"
    
    # 混淆错误
    OBFUSCATION_FAILED = "OBF_001"
    LAYER_INIT_FAILED = "OBF_002"
    DATA_ENCODING_FAILED = "OBF_003"
    ARITHMETIC_ERROR = "OBF_004"
    CONTROL_FLOW_ERROR = "OBF_005"
    
    # 解密错误
    DECRYPTION_FAILED = "DEC_001"
    INVALID_KEY = "DEC_002"
    INVALID_DATA_FORMAT = "DEC_003"
    
    # 虚拟机错误
    VM_INIT_FAILED = "VM_001"
    VM_EXECUTION_ERROR = "VM_002"
    BYTECODE_ERROR = "VM_003"
    STACK_OVERFLOW = "VM_004"
    
    # 推理错误
    MODEL_LOAD_FAILED = "INF_001"
    MODEL_NOT_FOUND = "INF_002"
    INFERENCE_FAILED = "INF_003"
    MEMORY_ERROR = "INF_004"
    
    # 配置错误
    CONFIG_NOT_FOUND = "CFG_001"
    CONFIG_PARSE_ERROR = "CFG_002"
    INVALID_PARAMETER = "CFG_003"
    MISSING_PARAMETER = "CFG_004"