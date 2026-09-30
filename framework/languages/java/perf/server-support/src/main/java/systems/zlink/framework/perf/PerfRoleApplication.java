package systems.zlink.framework.perf;

import org.springframework.boot.SpringBootConfiguration;
import org.springframework.boot.autoconfigure.EnableAutoConfiguration;

import systems.zlink.framework.spring.EnableZLinkFramework;

// The Spring root of every perf role: the Framework starter owns the host lifecycle; the role's beans and its
// ZLinkFrameworkConfigurer are registered by ServerApplication (no component scan, no handler package scan).
@SpringBootConfiguration(proxyBeanMethods = false)
@EnableAutoConfiguration
@EnableZLinkFramework
public class PerfRoleApplication {}
