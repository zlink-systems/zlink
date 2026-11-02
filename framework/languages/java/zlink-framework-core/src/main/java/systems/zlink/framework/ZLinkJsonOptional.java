/* SPDX-License-Identifier: Apache-2.0 */
package systems.zlink.framework;

import java.lang.annotation.ElementType;
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;
import java.lang.annotation.Target;

/** Marks a Java record component as omittable in framework-json-v1 payloads. */
@Retention(RetentionPolicy.RUNTIME)
@Target(ElementType.RECORD_COMPONENT)
public @interface ZLinkJsonOptional {}
